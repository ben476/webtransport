// Copyright (c) 2022 Marten Richter or other contributers (see commit). All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// portions taken from libquiche or Chromium, original copyright, see LICENSE.chromium
// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/http3client.h"
#include "src/http3eventloop.h"
#include "src/http3clientsession.h"
#include "src/http3wtsessionvisitor.h"
#include "src/http3sessioncache.h"

#include <memory>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "openssl/x509.h"
#include "quiche/quic/core/crypto/proof_verifier.h"
#include "quiche/quic/core/http/quic_spdy_client_stream.h"
#include "quiche/quic/core/http/spdy_utils.h"
#include "quiche/quic/core/http/web_transport_http3.h"
#include "quiche/quic/core/quic_epoll_connection_helper.h"
#include "quiche/quic/core/quic_epoll_alarm_factory.h"
#include "quiche/quic/core/web_transport_interface.h"
#include "quiche/quic/core/quic_packet_writer_wrapper.h"
#include "quiche/quic/core/quic_server_id.h"
#include "quiche/quic/core/quic_utils.h"
#include "quiche/quic/platform/api/quic_flags.h"
#include "quiche/quic/platform/api/quic_logging.h"
#include "quiche/quic/platform/api/quic_stack_trace.h"
//#include "quiche/quic/test_tools/crypto_test_utils.h"
#include "quiche/quic/core/quic_udp_socket.h"
#include "quiche/quic/tools/quic_url.h"
#include "quiche/common/quiche_text_utils.h"

using spdy::Http2HeaderBlock;

namespace quic
{

    // taken from chromium to behave like the browser
    // A version of WebTransportFingerprintProofVerifier that enforces
    // Chromium-specific policies.
    class ChromiumWebTransportFingerprintProofVerifier
        : public quic::WebTransportFingerprintProofVerifier
    {
    public:
        using WebTransportFingerprintProofVerifier::
            WebTransportFingerprintProofVerifier;

    protected:
        bool IsKeyTypeAllowedByPolicy(
            const quic::CertificateView &certificate) override
        {
            if (certificate.public_key_type() == quic::PublicKeyType::kRsa)
            {
                return false;
            }
            return WebTransportFingerprintProofVerifier::IsKeyTypeAllowedByPolicy(
                certificate);
        }
    };

    QuicStreamId GetNthClientInitiatedBidirectionalStreamId(
        QuicTransportVersion version, int n)
    {
        int num = n;
        if (!VersionUsesHttp3(version))
        {
            num++;
        }
        return QuicUtils::GetFirstBidirectionalStreamId(version,
                                                        Perspective::IS_CLIENT) +
               QuicUtils::StreamIdDelta(version) * num;
    }

    Http3Client::Http3Client(Http3EventLoop *eventloop,
                             QuicSocketAddress server_address, const std::string &server_hostname,
                             int local_port,
                             std::unique_ptr<ProofVerifier> proof_verifier,
                             std::unique_ptr<SessionCache> session_cache,
                             std::unique_ptr<QuicConnectionHelperInterface> helper)
        : server_id_(QuicServerId(server_hostname, server_address.port(), false)),
          initialized_(false),
          local_port_(local_port),
          store_response_(false),
          latest_response_code_(-1),
          overflow_supported_(false),
          packets_dropped_(0),
          packet_reader_(new QuicPacketReader()),
          crypto_config_(std::move(proof_verifier), std::move(session_cache)),
          helper_(std::move(helper)),
          eventloop_(eventloop),
          alarm_factory_(new QuicEpollAlarmFactory(eventloop->getEpollServer())),
          supported_versions_({ParsedQuicVersion::RFCv1()}),
          initial_max_packet_length_(0),
          num_sent_client_hellos_(0),
          connection_error_(QUIC_NO_ERROR),
          connected_or_attempting_connect_(false),
          server_connection_id_length_(kQuicDefaultConnectionIdLength),
          client_connection_id_length_(0),
          max_reads_per_epoll_loop_(std::numeric_limits<int>::max()),
          wait_for_encryption_(false),
          connection_in_progress_(false),
          num_attempts_connect_(0),
          webtransport_server_support_inform_(false),
          connection_debug_visitor_(nullptr)
    {
        set_server_address(server_address);
        Initialize();
    }

    Http3Client::~Http3Client()
    {
        // printf("client destruct %x\n", this);
    }

    bool Http3Client::closeClientInt()
    {
        for (std::pair<QuicStreamId, QuicSpdyClientStream *> stream : open_streams_)
        {
            stream.second->set_visitor(nullptr);
        }
        if (connected())
        {
            session_->connection()->CloseConnection(
                QUIC_PEER_GOING_AWAY, "Client being torn down",
                ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET);
        }
        // We own the push promise index. We need to explicitly kill
        // the session before the push promise index goes out of scope.
        ResetSession();

        CleanUpAllUDPSockets();

        eventloop_->informUnref(this);
        return true;
    }

    void Http3Client::Initialize()
    {
        priority_ = 3;
        connect_attempted_ = false;
        auto_reconnect_ = false;
        buffer_body_ = true;
        num_requests_ = 0;
        num_responses_ = 0;
        ClearPerConnectionState();
        // As chrome will generally do this, we want it to be the default when it's
        // not overridden.
        if (!config_.HasSetBytesForConnectionIdToSend())
        {
            config_.SetBytesForConnectionIdToSend(0);
        }
    }

    void Http3Client::SetUserAgentID(const std::string &user_agent_id)
    {
        crypto_config_.set_user_agent_id(user_agent_id);
    }

    void Http3Client::SendRequest(const std::string &uri)
    {
        spdy::SpdyHeaderBlock headers;
        if (!PopulateHeaderBlockFromUrl(uri, &headers))
        {
            return;
        }
        SendMessageAsync(headers, "");
    }

    void Http3Client::SendRequestAndRstTogether(const std::string &uri)
    {
        spdy::SpdyHeaderBlock headers;
        if (!PopulateHeaderBlockFromUrl(uri, &headers))
        {
            return;
        }

        QuicSpdyClientSession *session = session_.get();
        QuicConnection::ScopedPacketFlusher flusher(session->connection());
        SendMessageAsync(headers, "", /*fin=*/true);

        QuicStreamId stream_id = GetNthClientInitiatedBidirectionalStreamId(
            session->transport_version(), 0);
        session->ResetStream(stream_id, QUIC_STREAM_CANCELLED);
    }

    void Http3Client::GetOrCreateStreamAndSendRequest(
        const spdy::SpdyHeaderBlock *headers, absl::string_view body, bool fin)
    {
        if (headers)
        {
            QuicClientPushPromiseIndex::TryHandle *handle;
            QuicAsyncStatus rv =
                push_promise_index_.Try(*headers, this, &handle);
            if (rv == QUIC_SUCCESS)
                return;
            if (rv == QUIC_PENDING)
            {
                // May need to retry request if asynchronous rendezvous fails.
                std::unique_ptr<spdy::SpdyHeaderBlock> new_headers(
                    new spdy::SpdyHeaderBlock(headers->Clone()));
                push_promise_data_to_resend_ = std::make_unique<Http3ClientDataToResend>(
                    std::move(new_headers), body, fin, this);
                return;
            }
        }

        std::shared_ptr<spdy::Http2HeaderBlock> spdy_headers;
        bool hasheaders = false;
        if (headers != nullptr)
        {
            spdy_headers = std::make_shared<spdy::SpdyHeaderBlock>(headers->Clone());
            hasheaders = true;
        }

        // Maybe it's better just to overload this.  it's just that we need
        // for the GetOrCreateStream function to call something else...which
        // is icky and complicated, but maybe not worse than this.
        RunOnStreamMaybeCreateStream(
            [spdy_headers, hasheaders, body, fin, this](QuicSpdyClientStream *stream)
            {
                if (stream == nullptr)
                {
                    return;
                }
                // QuicSpdyStreamPeer::set_ack_listener(stream, ack_listener);

                ssize_t ret = 0;
                if (hasheaders)
                {
                    if ((*spdy_headers.get())[":authority"].as_string().empty())
                    {
                        (*spdy_headers.get())[":authority"] = server_id_.host();
                    }
                    ret = stream->SendRequest(std::move(*spdy_headers.get()), body, fin);
                    ++num_requests_;
                }
                else
                {
                    stream->WriteOrBufferBody(std::string(body), fin);
                    ret = body.length();
                }
            });
    }

    void Http3Client::SendMessageAsync(const spdy::SpdyHeaderBlock &headers,
                                       absl::string_view body)
    {
        return SendMessageAsync(headers, body, /*fin=*/true);
    }

    void Http3Client::SendMessageAsync(const spdy::SpdyHeaderBlock &headers,
                                       absl::string_view body, bool fin)
    {
        // Always force creation of a stream for SendMessage.
        latest_created_stream_ = nullptr;

        GetOrCreateStreamAndSendRequest(&headers, body, fin);
    }

    bool Http3Client::response_complete() const { return response_complete_; }

    int64_t Http3Client::response_body_size() const
    {
        return response_body_size_;
    }

    bool Http3Client::buffer_body() const { return buffer_body_; }

    void Http3Client::set_buffer_body(bool buffer_body)
    {
        buffer_body_ = buffer_body;
    }

    const std::string &Http3Client::response_body() const { return response_; }

    void Http3Client::SendConnectivityProbing()
    {
        QuicConnection *connection = session_->connection();
        connection->SendConnectivityProbingPacket(connection->writer(),
                                                  connection->peer_address());
    }

    void Http3Client::SetLatestCreatedStream(QuicSpdyClientStream *stream)
    {
        latest_created_stream_ = stream;
        if (latest_created_stream_ != nullptr)
        {
            open_streams_[stream->id()] = stream;
            stream->set_visitor(this);
        }
    }

    void Http3Client::CreateClientStream(std::function<void(QuicSpdyClientStream *)> finish)
    {
        if (!connected())
        {
            finish(nullptr);
        }
        finish_stream_open_.push(finish);
        /*
        if (VersionHasIetfQuicFrames(session_->transport_version()))
        {
            // Process MAX_STREAMS from peer or wait for liveness testing succeeds.
            while (!session_->CanOpenNextOutgoingBidirectionalStream())
            {
                RunEventLoop();
            }
        }
        auto *stream = static_cast<QuicSpdyClientStream *>(
            session_->CreateOutgoingBidirectionalStream());
        if (stream)
        {
            stream->set_visitor(this);
        }
        return stream; */
    }

    void Http3Client::RunOnStreamMaybeCreateStream(std::function<void(QuicSpdyClientStream *)> finish)
    {
        if (!connect_attempted_ || auto_reconnect_)
        {
            if (!connected())
            {
                Connect();
            }
            if (!connected())
            {
                finish(nullptr);
                return;
            }
        }
        if (open_streams_.empty())
        {
            ClearPerConnectionState();
        }
        if (!latest_created_stream_)
        {
            CreateClientStream(
                [this, finish](QuicSpdyClientStream *stream)
                {
                    SetLatestCreatedStream(stream);
                    if (latest_created_stream_)
                    {
                        latest_created_stream_->SetPriority(
                            spdy::SpdyStreamPrecedence(priority_));
                    }
                    finish(latest_created_stream_);
                });
        }
        else
        {
            finish(latest_created_stream_);
        }
    }

    QuicErrorCode Http3Client::connection_error() const
    {
        // Return the high-level error if there was one.  Otherwise, return the
        // connection error from the last session.
        if (connection_error_ != QUIC_NO_ERROR)
        {
            return connection_error_;
        }
        if (session_ == nullptr)
        {
            return QUIC_NO_ERROR;
        }
        return session_->error();
    }

    const QuicTagValueMap &Http3Client::GetServerConfig()
    {
        const QuicCryptoClientConfig::CachedState *state =
            crypto_config_.LookupOrCreate(server_id_);
        const CryptoHandshakeMessage *handshake_msg = state->GetServerConfig();
        return handshake_msg->tag_value_map();
    }

    bool Http3Client::connected() const
    {
        return session_.get() && session_->connection() &&
               session_->connection()->connected();
    }

    bool Http3Client::clientInitialize()
    {
        num_sent_client_hellos_ = 0;
        connection_error_ = QUIC_NO_ERROR;
        connected_or_attempting_connect_ = false;

        // If an initial flow control window has not explicitly been set, then use the
        // same values that Chrome uses.
        const uint32_t kSessionMaxRecvWindowSize = 15 * 1024 * 1024; // 15 MB
        const uint32_t kStreamMaxRecvWindowSize = 6 * 1024 * 1024;   //  6 MB
        if (config_.GetInitialStreamFlowControlWindowToSend() ==
            kDefaultFlowControlSendWindow)
        {
            config_.SetInitialStreamFlowControlWindowToSend(kStreamMaxRecvWindowSize);
        }
        if (config_.GetInitialSessionFlowControlWindowToSend() ==
            kDefaultFlowControlSendWindow)
        {
            config_.SetInitialSessionFlowControlWindowToSend(
                kSessionMaxRecvWindowSize);
        }

        if (!CreateUDPSocketAndBind(server_address_,
                                    bind_to_address_, local_port_))
        {
            return false;
        }

        initialized_ = true;
        return true;
    }

    bool Http3Client::CreateUDPSocketAndBind(
        QuicSocketAddress server_address, QuicIpAddress bind_to_address,
        int bind_to_port)
    {
        eventloop_->getEpollServer()->set_timeout_in_us(50 * 1000);

        QuicUdpSocketApi api;
        int fd = api.Create(server_address.host().AddressFamilyToInt(),
                            /*receive_buffer_size =*/kDefaultSocketReceiveBuffer,
                            /*send_buffer_size =*/kDefaultSocketReceiveBuffer);
        if (fd < 0)
        {
            return false;
        }

        overflow_supported_ = api.EnableDroppedPacketCount(fd);
        api.EnableReceiveTimestamp(fd);

        QuicSocketAddress client_address;
        if (bind_to_address.IsInitialized())
        {
            client_address = QuicSocketAddress(bind_to_address, local_port_);
        }
        else if (server_address.host().address_family() == IpAddressFamily::IP_V4)
        {
            client_address = QuicSocketAddress(QuicIpAddress::Any4(), bind_to_port);
        }
        else
        {
            client_address = QuicSocketAddress(QuicIpAddress::Any6(), bind_to_port);
        }

        // Some platforms expect that the addrlen given to bind() exactly matches the
        // size of the associated protocol family's sockaddr struct.
        // TODO(b/179430548): Revert this when affected platforms are updated to
        // to support binding with an addrelen of sizeof(sockaddr_storage)
        socklen_t addrlen;
        switch (client_address.host().address_family())
        {
        case IpAddressFamily::IP_V4:
            addrlen = sizeof(sockaddr_in);
            break;
        case IpAddressFamily::IP_V6:
            addrlen = sizeof(sockaddr_in6);
            break;
        case IpAddressFamily::IP_UNSPEC:
            addrlen = 0;
            break;
        }

        sockaddr_storage addr = client_address.generic_address();
        int rc = bind(fd, reinterpret_cast<sockaddr *>(&addr), addrlen);
        if (rc < 0)
        {
            QUIC_LOG(ERROR) << "Bind failed: " << strerror(errno)
                            << " bind_to_address:" << bind_to_address
                            << ", bind_to_port:" << bind_to_port
                            << ", client_address:" << client_address;
            return false;
        }

        if (client_address.FromSocket(fd) != 0)
        {
            QUIC_LOG(ERROR) << "Unable to get self address.  Error: "
                            << strerror(errno);
        }
        const int kEpollFlags = UV_READABLE | UV_WRITABLE; // there is no analogue to EPOLLET in libuv hopefully not a problem

        fd_address_map_[fd] = client_address;
        eventloop_->getEpollServer()->RegisterFD(fd, this, kEpollFlags);
        return true;
    }

    void Http3Client::CleanUpUDPSocket(int fd)
    {
        CleanUpUDPSocketImpl(fd);
        fd_address_map_.erase(fd);
    }

    void Http3Client::CleanUpAllUDPSockets()
    {
        for (std::pair<int, QuicSocketAddress> fd_address : fd_address_map_)
        {
            CleanUpUDPSocketImpl(fd_address.first);
        }
        fd_address_map_.clear();
    }

    void Http3Client::CleanUpUDPSocketImpl(int fd)
    {
        if (fd > -1)
        {
            eventloop_->getEpollServer()->UnregisterFD(fd);
            int rc = close(fd);
            QUICHE_DCHECK_EQ(0, rc);
        }
    }

    QuicSocketAddress Http3Client::GetLatestClientAddress() const
    {
        if (fd_address_map_.empty())
        {
            return QuicSocketAddress();
        }

        return fd_address_map_.back().second;
    }

    void Http3Client::Connect()
    {
        if (connected())
        {
            QUIC_BUG(quic_bug_10133_1) << "Cannot connect already-connected client";
            return;
        }
        if (!connect_attempted_)
        {
            clientInitialize();
        }

        // If we've been asked to override SNI, set it now
        if (override_sni_set_)
        {
            // This should only be set before the initial Connect()
            server_id_ = QuicServerId(override_sni_, address().port(), false);
        }
        connection_in_progress_ = true;
        wait_for_encryption_ = false;

        /*
                // TODO, we have to use the event loop for this!

                // Attempt multiple connects until the maximum number of client hellos have
                // been sent.
                int num_attempts = 0;
                while (!connected() &&
                       num_attempts <= QuicCryptoClientStream::kMaxClientHellos)
                {
                    StartConnect();
                    while (EncryptionBeingEstablished())
                    {
                        WaitForEvents();
                    }
                    ParsedQuicVersion version = UnsupportedQuicVersion();
                    if (session_ != nullptr && !CanReconnectWithDifferentVersion(&version))
                    {
                        // We've successfully created a session but we're not connected, and we
                        // cannot reconnect with a different version.  Give up trying.
                        break;
                    }
                    num_attempts++;
                }
                if (session_ == nullptr)
                {
                    QUIC_BUG(quic_bug_10906_1) << "Missing session after Connect";
                    return;
                }
                connect_attempted_ = true;
                */
    }

    bool Http3Client::handleConnecting()
    {
        bool recheck = false;
        if (connection_in_progress_)
        {
            if (!wait_for_encryption_)
            {
                if (!connected() && num_attempts_connect_ <= QuicCryptoClientStream::kMaxClientHellos)
                {
                    StartConnect();
                    wait_for_encryption_ = true;
                }
                else if (session_ == nullptr && num_attempts_connect_ > QuicCryptoClientStream::kMaxClientHellos)
                {
                    connection_in_progress_ = false;
                    connect_attempted_ = true;
                    QUIC_BUG(quic_bug_10906_1) << "Missing session after Connect";
                    eventloop_->informAboutClientConnected(this, false);
                }
            }
            if (wait_for_encryption_)
            {
                if (EncryptionBeingEstablished())
                    return false;
                wait_for_encryption_ = false;
                ParsedQuicVersion version = UnsupportedQuicVersion();
                if (session_ != nullptr && !CanReconnectWithDifferentVersion(&version) && !session_->connection()->connected())
                {
                    // We've successfully created a session but we're not connected, and we
                    // cannot reconnect with a different version.  Give up trying.
                    connection_in_progress_ = false;
                    connect_attempted_ = true;
                    eventloop_->informAboutClientConnected(this, false);
                }
                else if (session_ != nullptr && session_->connection()->connected())
                {
                    connect_attempted_ = true;
                    connection_in_progress_ = false;
                    eventloop_->informAboutClientConnected(this, true);
                    webtransport_server_support_inform_ = true;
                    recheck = true;
                }
                else
                {
                    num_attempts_connect_++;
                    recheck = true;
                }
            }
        }
        if (webtransport_server_support_inform_ && connected())
        {
            if (session_->SupportsWebTransport())
            {
                eventloop_->informClientWebtransportSupport(this);
                webtransport_server_support_inform_ = false;
            }
            else
                recheck = true;
        }

        while (finish_stream_open_.size() > 0 && session_->CanOpenNextOutgoingBidirectionalStream())
        {
            auto *stream = static_cast<QuicSpdyClientStream *>(
                session_->CreateOutgoingBidirectionalStream());
            if (stream)
            {
                stream->set_visitor(this);
            }
            finish_stream_open_.front()(stream);
            if (stream != nullptr)
            {
                if (stream->web_transport() != nullptr)
                {
                    WebTransportSessionId id = stream->id();
                    WebTransportHttp3 *wtsession = session_->GetWebTransportSession(id);
                    if (wtsession == nullptr)
                    {
                        eventloop_->informNewClientSession(this, nullptr);
                        // may be throw error
                    }
                    else
                    {
                        // ok we have our session, do we wait for session ready, no set visitor immediatele
                        Http3WTSession *wtsessionobj =
                            new Http3WTSession(
                                static_cast<WebTransportSession *>(wtsession),
                                eventloop_);
                        eventloop_->informNewClientSession(this, wtsessionobj);
                        auto visitor = std::make_unique<Http3WTSession::Visitor>(wtsessionobj);
                        wtsession->SetVisitor(std::move(visitor));
                    }
                }
            }
            finish_stream_open_.pop();
        }

        return recheck;
    }

    void Http3Client::openWTSessionInt(absl::string_view path)
    {
        spdy::SpdyHeaderBlock headers;
        headers[":scheme"] = "https";
        headers[":authority"] = "localhost";
        headers[":path"] = path;
        headers[":method"] = "CONNECT";
        headers[":protocol"] = "webtransport";

        SendMessageAsync(headers, "", /*fin=*/false);
    }

    int Http3Client::GetLatestFD() const
    {
        if (fd_address_map_.empty())
        {
            return -1;
        }

        return fd_address_map_.back().first;
    }

    void Http3Client::StartConnect()
    {
        QUICHE_DCHECK(initialized_);
        QUICHE_DCHECK(!connected());
        QuicPacketWriter *writer = new QuicDefaultPacketWriter(GetLatestFD());
        ParsedQuicVersion mutual_version = UnsupportedQuicVersion();
        const bool can_reconnect_with_different_version =
            CanReconnectWithDifferentVersion(&mutual_version);
        if (connected_or_attempting_connect_)
        {
            // Clear queued up data if client can not try to connect with a different
            // version.
            if (!can_reconnect_with_different_version)
            {
                ClearDataToResend();
            }
            // Before we destroy the last session and create a new one, gather its stats
            // and update the stats for the overall connection.
            // no stats for me!
            // UpdateStats();
        }
        QuicConnectionId newconnid = QuicUtils::CreateRandomConnectionId(server_connection_id_length_);

        const quic::ParsedQuicVersionVector client_supported_versions =
            can_reconnect_with_different_version
                ? ParsedQuicVersionVector{mutual_version}
                : supported_versions_;

        session_ = std::make_unique<Http3ClientSession>(
            config_, client_supported_versions, new QuicConnection(newconnid, QuicSocketAddress(), server_address_, helper_.get(), alarm_factory_.get(), writer,
                                                                   /* owns_writer= */ false, Perspective::IS_CLIENT, client_supported_versions),
            server_id_, &crypto_config_,
            &push_promise_index_, false /*drop_response_body_*/, true /* enable_web_transport */);

        if (can_reconnect_with_different_version)
        {
            session_->set_client_original_supported_versions(supported_versions_);
        }
        if (connection_debug_visitor_ != nullptr)
        {
            session_->connection()->set_debug_visitor(connection_debug_visitor_);
        }
        session_->connection()->set_client_connection_id(
            QuicUtils::CreateRandomConnectionId(client_connection_id_length_));
        if (initial_max_packet_length_ != 0)
        {
            session_->connection()->SetMaxPacketLength(initial_max_packet_length_);
        }
        // Reset |writer()| after |session()| so that the old writer outlives the old
        // session.
        if (writer_.get() != writer)
        {
            writer_.reset(writer);
        }
        // set_writer(writer);
        InitializeSession();
        if (can_reconnect_with_different_version)
        {
            // This is a reconnect using server supported |mutual_version|.
            session_->connection()->SetVersionNegotiated();
        }
        connected_or_attempting_connect_ = true;
    }

    void Http3Client::InitializeSession()
    {
        if (max_inbound_header_list_size_ > 0)
        {
            session_->set_max_inbound_header_list_size(
                max_inbound_header_list_size_);
        }
        session_->Initialize();
        session_->CryptoConnect();
    }

    void Http3Client::ResetConnection()
    {
        Disconnect();
        Connect();
    }

    void Http3Client::Disconnect()
    {
        ClearPerConnectionState();
        if (initialized_)
        {
            QUICHE_DCHECK(initialized_);

            initialized_ = false;
            if (connected())
            {
                session_->connection()->CloseConnection(
                    QUIC_PEER_GOING_AWAY, "Client disconnecting",
                    ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET);
            }

            ClearDataToResend();

            CleanUpAllUDPSockets();
        }
        connect_attempted_ = false;
    }

    bool Http3Client::CanReconnectWithDifferentVersion(
        ParsedQuicVersion *version) const
    {
        if (session_ == nullptr || session_->connection() == nullptr ||
            session_->error() != QUIC_INVALID_VERSION)
        {
            return false;
        }

        const auto &server_supported_versions =
            session_->connection()->server_supported_versions();
        if (server_supported_versions.empty())
        {
            return false;
        }

        for (const auto &client_version : supported_versions_)
        {
            if (std::find(server_supported_versions.begin(),
                          server_supported_versions.end(),
                          client_version) != server_supported_versions.end())
            {
                *version = client_version;
                return true;
            }
        }
        return false;
    }

    bool Http3Client::EncryptionBeingEstablished()
    {
        return !session_->IsEncryptionEstablished() &&
               session_->connection()->connected();
    }

    bool Http3Client::HasActiveRequests()
    {
        return session_->HasActiveRequestStreams();
    }

    QuicSocketAddress Http3Client::local_address() const
    {
        return GetLatestClientAddress();
    }

    void Http3Client::ClearPerRequestState()
    {
        stream_error_ = QUIC_STREAM_NO_ERROR;
        response_ = "";
        response_complete_ = false;
        response_headers_complete_ = false;
        preliminary_headers_.clear();
        response_headers_.clear();
        response_trailers_.clear();
        bytes_read_ = 0;
        bytes_written_ = 0;
        response_body_size_ = 0;
    }

    void Http3Client::ClearDataToResend()
    {
        data_to_resend_on_connect_.clear();
    }

    bool Http3Client::HaveActiveStream()
    {
        return push_promise_data_to_resend_.get() || !open_streams_.empty();
    }

    void Http3Client::OnRegistration(QuicEpollServer * /*eps*/,
                                     int /*fd*/,
                                     int /*event_mask*/) {}
    void Http3Client::OnModification(int /*fd*/,
                                     int /*event_mask*/) {}
    void Http3Client::OnUnregistration(int /*fd*/,
                                       bool /*replaced*/) {}
    void Http3Client::OnShutdown(QuicEpollServer * /*eps*/,
                                 int /*fd*/) {}

    void Http3Client::OnEvent(int fd, QuicEpollEvent *event)
    {
        event->out_ready_mask = 0; // special
        if (event->in_events & UV_READABLE)
        {
            QUIC_DVLOG(1) << "Read packets on UV_READABLE";
            int times_to_read = max_reads_per_epoll_loop_;
            bool more_to_read = true;
            QuicPacketCount packets_dropped = 0;
            while (connected() && more_to_read && times_to_read > 0)
            {
                more_to_read = packet_reader_->ReadAndDispatchPackets(
                    fd, GetLatestClientAddress().port(), *helper_->GetClock(),
                    this, overflow_supported_ ? &packets_dropped : nullptr);
                --times_to_read;
            }
            if (packets_dropped_ < packets_dropped)
            {
                QUIC_LOG(ERROR)
                    << packets_dropped - packets_dropped_
                    << " more packets are dropped in the socket receive buffer.";
                packets_dropped_ = packets_dropped;
            }
            if (connected() && more_to_read)
            {
                event->out_ready_mask |= UV_READABLE;
            }
        }
        if (connected() && (event->in_events & UV_WRITABLE))
        {
            writer_->SetWritable();
            session_->connection()->OnCanWrite();
            if (writer_->IsWriteBlocked())
            {
                event->out_ready_mask |= UV_WRITABLE;
            }
        }

        if (handleConnecting())
        {
            event->out_ready_mask |= UV_READABLE; // please visit us again
        }
        /*  if (event->in_events & EPOLLERR)
          {
              QUIC_DLOG(INFO) << "Epollerr";
          } */
    }

    void Http3Client::ProcessPacket(
        const QuicSocketAddress &self_address,
        const QuicSocketAddress &peer_address, const QuicReceivedPacket &packet)
    {
        session_->ProcessUdpPacket(self_address, peer_address, packet);
    }

    bool Http3Client::response_headers_complete() const
    {
        for (std::pair<QuicStreamId, QuicSpdyClientStream *> stream : open_streams_)
        {
            if (stream.second->headers_decompressed())
            {
                return true;
            }
        }
        return response_headers_complete_;
    }

    const spdy::SpdyHeaderBlock *Http3Client::response_headers() const
    {
        for (std::pair<QuicStreamId, QuicSpdyClientStream *> stream : open_streams_)
        {
            if (stream.second->headers_decompressed())
            {
                response_headers_ = stream.second->response_headers().Clone();
                break;
            }
        }
        return &response_headers_;
    }

    const spdy::SpdyHeaderBlock *Http3Client::preliminary_headers() const
    {
        for (std::pair<QuicStreamId, QuicSpdyClientStream *> stream : open_streams_)
        {
            size_t bytes_read =
                stream.second->stream_bytes_read() + stream.second->header_bytes_read();
            if (bytes_read > 0)
            {
                preliminary_headers_ = stream.second->preliminary_headers().Clone();
                break;
            }
        }
        return &preliminary_headers_;
    }

    const spdy::SpdyHeaderBlock &Http3Client::response_trailers() const
    {
        return response_trailers_;
    }

    int64_t Http3Client::response_size() const { return bytes_read(); }

    size_t Http3Client::bytes_read() const
    {
        for (std::pair<QuicStreamId, QuicSpdyClientStream *> stream : open_streams_)
        {
            size_t bytes_read = stream.second->total_body_bytes_read() +
                                stream.second->header_bytes_read();
            if (bytes_read > 0)
            {
                return bytes_read;
            }
        }
        return bytes_read_;
    }

    size_t Http3Client::bytes_written() const
    {
        for (std::pair<QuicStreamId, QuicSpdyClientStream *> stream : open_streams_)
        {
            size_t bytes_written = stream.second->stream_bytes_written() +
                                   stream.second->header_bytes_written();
            if (bytes_written > 0)
            {
                return bytes_written;
            }
        }
        return bytes_written_;
    }

    void Http3Client::OnCompleteResponse(
        QuicStreamId id, const spdy::Http2HeaderBlock &response_headers,
        const std::string &response_body)
    {
        // implement
    }

    void Http3Client::OnClose(QuicSpdyStream *stream)
    {
        if (stream == nullptr)
        {
            return;
        }
        // Always close the stream, regardless of whether it was the last stream
        // written.
        QUICHE_DCHECK(stream != nullptr);
        QuicSpdyClientStream *client_stream =
            static_cast<QuicSpdyClientStream *>(stream);

        const Http2HeaderBlock &response_headers = client_stream->response_headers();

        OnCompleteResponse(stream->id(), response_headers, client_stream->data());

        // Store response headers and body.
        if (store_response_)
        {
            auto status = response_headers.find(":status");
            if (status == response_headers.end())
            {
                QUIC_LOG(ERROR) << "Missing :status response header";
            }
            else if (!absl::SimpleAtoi(status->second, &latest_response_code_))
            {
                QUIC_LOG(ERROR) << "Invalid :status response header: " << status->second;
            }
            latest_response_headers_ = response_headers.DebugString();
            preliminary_response_headers_ =
                client_stream->preliminary_headers().DebugString();
            latest_response_header_block_ = response_headers.Clone();
            latest_response_body_ = client_stream->data();
            latest_response_trailers_ =
                client_stream->received_trailers().DebugString();
        }
        ++num_responses_;
        if (open_streams_.find(stream->id()) == open_streams_.end())
        {
            return;
        }
        if (latest_created_stream_ == stream)
        {
            latest_created_stream_ = nullptr;
        }

        QuicStreamId id = client_stream->id();
        closed_stream_states_.insert(std::make_pair(
            id,
            PerStreamState(
                // Set response_complete to true iff stream is closed while connected.
                client_stream->stream_error(), connected(),
                client_stream->headers_decompressed(),
                client_stream->response_headers(),
                client_stream->preliminary_headers(),
                (buffer_body() ? client_stream->data() : ""),
                client_stream->received_trailers(),
                // Use NumBytesConsumed to avoid counting retransmitted stream frames.
                client_stream->total_body_bytes_read() +
                    client_stream->header_bytes_read(),
                client_stream->stream_bytes_written() +
                    client_stream->header_bytes_written(),
                client_stream->data().size())));
        open_streams_.erase(id);
    }

    bool Http3Client::CheckVary(
        const spdy::SpdyHeaderBlock & /*client_request*/,
        const spdy::SpdyHeaderBlock & /*promise_request*/,
        const spdy::SpdyHeaderBlock & /*promise_response*/)
    {
        return true;
    }

    void Http3Client::OnRendezvousResult(QuicSpdyStream *stream)
    {
        std::unique_ptr<Http3ClientDataToResend> data_to_resend =
            std::move(push_promise_data_to_resend_);
        SetLatestCreatedStream(static_cast<QuicSpdyClientStream *>(stream));
        if (stream)
        {
            stream->OnBodyAvailable();
        }
        else if (data_to_resend)
        {
            data_to_resend->Resend();
        }
    }

    bool Http3Client::MigrateSocket(const QuicIpAddress &new_host)
    {
        return MigrateSocketWithSpecifiedPort(new_host, local_port_);
    }

    bool Http3Client::MigrateSocketWithSpecifiedPort(
        const QuicIpAddress &new_host, int port)
    {
        local_port_ = port;
        if (!connected())
        {
            QUICHE_DVLOG(1)
                << "MigrateSocketWithSpecifiedPort failed as connection has closed";
            return false;
        }

        CleanUpAllUDPSockets();
        std::unique_ptr<QuicPacketWriter> writer =
            CreateWriterForNewNetwork(new_host, port);
        if (writer == nullptr)
        {
            QUICHE_DVLOG(1)
                << "MigrateSocketWithSpecifiedPort failed from writer creation";
            return false;
        }
        if (!session_->MigratePath(GetLatestClientAddress(),
                                   session_->connection()->peer_address(),
                                   writer.get(), false))
        {
            QUICHE_DVLOG(1)
                << "MigrateSocketWithSpecifiedPort failed from session()->MigratePath";
            return false;
        }
        writer_ = std::move(writer);
        // set_writer(writer.release());
        return true;
    }

    std::unique_ptr<QuicPacketWriter> Http3Client::CreateWriterForNewNetwork(
        const QuicIpAddress &new_host, int port)
    {
        set_bind_to_address(new_host);
        local_port_ = port;
        if (!CreateUDPSocketAndBind(server_address_,
                                    bind_to_address_, port))
        {
            return nullptr;
        }

        QuicPacketWriter *writer = new QuicDefaultPacketWriter(GetLatestFD());
        QUIC_LOG_IF(WARNING, writer == writer_.get())
            << "The new writer is wrapped in the same wrapper as the old one, thus "
               "appearing to have the same address as the old one.";
        return std::unique_ptr<QuicPacketWriter>(writer);
    }

    QuicIpAddress Http3Client::bind_to_address() const
    {
        return bind_to_address_;
    }

    void Http3Client::set_bind_to_address(QuicIpAddress address)
    {
        bind_to_address_ = address;
    }

    const QuicSocketAddress &Http3Client::address() const
    {
        return server_address_;
    }

    Http3Client::Http3ClientDataToResend::Http3ClientDataToResend(
        std::unique_ptr<spdy::SpdyHeaderBlock> headers, absl::string_view body,
        bool fin, Http3Client *client)
        : headers_(std::move(headers)), body_(body), fin_(fin),
          client_(client) {}

    Http3Client::Http3ClientDataToResend::~Http3ClientDataToResend() = default;

    void Http3Client::Http3ClientDataToResend::Resend()
    {
        client_->GetOrCreateStreamAndSendRequest(headers_.get(), body_, fin_);
        headers_.reset();
    }

    Http3Client::PerStreamState::PerStreamState(const PerStreamState &other)
        : stream_error(other.stream_error),
          response_complete(other.response_complete),
          response_headers_complete(other.response_headers_complete),
          response_headers(other.response_headers.Clone()),
          preliminary_headers(other.preliminary_headers.Clone()),
          response(other.response),
          response_trailers(other.response_trailers.Clone()),
          bytes_read(other.bytes_read),
          bytes_written(other.bytes_written),
          response_body_size(other.response_body_size) {}

    Http3Client::PerStreamState::PerStreamState(
        QuicRstStreamErrorCode stream_error, bool response_complete,
        bool response_headers_complete,
        const spdy::SpdyHeaderBlock &response_headers,
        const spdy::SpdyHeaderBlock &preliminary_headers,
        const std::string &response, const spdy::SpdyHeaderBlock &response_trailers,
        uint64_t bytes_read, uint64_t bytes_written, int64_t response_body_size)
        : stream_error(stream_error),
          response_complete(response_complete),
          response_headers_complete(response_headers_complete),
          response_headers(response_headers.Clone()),
          preliminary_headers(preliminary_headers.Clone()),
          response(response),
          response_trailers(response_trailers.Clone()),
          bytes_read(bytes_read),
          bytes_written(bytes_written),
          response_body_size(response_body_size) {}

    Http3Client::PerStreamState::~PerStreamState() = default;

    bool Http3Client::PopulateHeaderBlockFromUrl(
        const std::string &uri, spdy::SpdyHeaderBlock *headers)
    {
        std::string url;
        if (absl::StartsWith(uri, "https://") || absl::StartsWith(uri, "http://"))
        {
            url = uri;
        }
        else if (uri[0] == '/')
        {
            url = "https://" + server_id_.host() + uri;
        }
        else
        {
            url = "https://" + uri;
        }
        return SpdyUtils::PopulateHeaderBlockFromUrl(url, headers);
    }

    /*
        void Http3Client::ReadNextResponse()
        {
            if (closed_stream_states_.empty())
            {
                return;
            }

            PerStreamState state(closed_stream_states_.front().second);

            stream_error_ = state.stream_error;
            response_ = state.response;
            response_complete_ = state.response_complete;
            response_headers_complete_ = state.response_headers_complete;
            preliminary_headers_ = state.preliminary_headers.Clone();
            response_headers_ = state.response_headers.Clone();
            response_trailers_ = state.response_trailers.Clone();
            bytes_read_ = state.bytes_read;
            bytes_written_ = state.bytes_written;
            response_body_size_ = state.response_body_size;

            closed_stream_states_.pop_front();
        }
        */

    void Http3Client::ClearPerConnectionState()
    {
        ClearPerRequestState();
        open_streams_.clear();
        closed_stream_states_.clear();
        latest_created_stream_ = nullptr;
    }

    NAN_METHOD(Http3Client::New)
    {
        if (info.IsConstructCall())
        {
            std::string port = "443";
            v8::Isolate *isolate = info.GetIsolate();
            bool allowPooling = false;
            std::vector<WebTransportHash> serverCertificateHashes;
            std::string privkey;
            std::string hostname = "localhost";
            int local_port = 0;

            v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();

            if (!info[0]->IsUndefined())
            {
                v8::MaybeLocal<v8::Object> obj = info[0]->ToObject(context);
                v8::Local<v8::Object> lobj = obj.ToLocalChecked();
                v8::Local<v8::String> poolProp = Nan::New("allowPooling").ToLocalChecked();
                v8::Local<v8::String> hashProp = Nan::New("serverCertificateHashes").ToLocalChecked();
                v8::Local<v8::String> algorithmProp = Nan::New("algorithm").ToLocalChecked();
                v8::Local<v8::String> valueProp = Nan::New("value").ToLocalChecked();
                v8::Local<v8::String> portProp = Nan::New("port").ToLocalChecked();
                v8::Local<v8::String> hostnameProp = Nan::New("hostname").ToLocalChecked();
                v8::Local<v8::String> localPortProp = Nan::New("localPort").ToLocalChecked();
                if (!obj.IsEmpty())
                {

                    if (Nan::HasOwnProperty(lobj, poolProp).FromJust() && !Nan::Get(lobj, poolProp).IsEmpty())
                    {
                        v8::Local<v8::Value> poolValue = Nan::Get(lobj, poolProp).ToLocalChecked();
                        allowPooling = Nan::To<bool>(poolValue).FromJust();
                    }

                    if (Nan::HasOwnProperty(lobj, portProp).FromJust() && !Nan::Get(lobj, portProp).IsEmpty())
                    {
                        v8::Local<v8::Value> portValue = Nan::Get(lobj, portProp).ToLocalChecked();
                        port = *v8::String::Utf8Value(isolate, portValue->ToString(context).ToLocalChecked());
                    }
                    else
                        return Nan::ThrowError("no port specified");

                    if (Nan::HasOwnProperty(lobj, hostnameProp).FromJust() && !Nan::Get(lobj, hostnameProp).IsEmpty())
                    {
                        v8::Local<v8::Value> hostnameValue = Nan::Get(lobj, hostnameProp).ToLocalChecked();
                        hostname = *v8::String::Utf8Value(isolate, hostnameValue->ToString(context).ToLocalChecked());
                    }
                    else
                        return Nan::ThrowError("no hostname specified");

                    if (Nan::HasOwnProperty(lobj, hashProp).FromJust() && !Nan::Get(lobj, hashProp).IsEmpty())
                    {
                        v8::Local<v8::Value> hashValue = Nan::Get(lobj, hashProp).ToLocalChecked();
                        if (hashValue->IsArray())
                        {
                            v8::Local<v8::Array> hashArray = v8::Local<v8::Array>::Cast(hashValue);
                            int length = hashArray->Length();

                            for (unsigned int i = 0; i < length; i++)
                            {
                                WebTransportHash curhash;
                                v8::Local<v8::Value> hash = hashArray->Get(context, i).ToLocalChecked();
                                v8::Local<v8::Object> hashobj = Nan::To<v8::Object>(hash).ToLocalChecked();
                                if (Nan::HasOwnProperty(hashobj, valueProp).FromJust() && !Nan::Get(hashobj, valueProp).IsEmpty())
                                {
                                    v8::Local<v8::Object> bufferlocal = Nan::To<v8::Object>(Nan::Get(hashobj, valueProp).ToLocalChecked()).ToLocalChecked();
                                    char *buffer = node::Buffer::Data(bufferlocal);
                                    size_t len = node::Buffer::Length(bufferlocal);
                                    curhash.value = std::string(buffer, len);
                                }
                                else
                                    return Nan::ThrowError("serverCertificateHashes wrong format");
                                if (Nan::HasOwnProperty(hashobj, algorithmProp).FromJust() && !Nan::Get(hashobj, algorithmProp).IsEmpty())
                                {
                                    v8::Local<v8::Value> algorithmValue = Nan::Get(hashobj, algorithmProp).ToLocalChecked();
                                    curhash.algorithm = *v8::String::Utf8Value(isolate, algorithmValue->ToString(context).ToLocalChecked());
                                }
                                else
                                    return Nan::ThrowError("serverCertificateHashes wrong format");
                                if (curhash.algorithm.compare(WebTransportHash::kSha256) != 0)
                                    return Nan::ThrowError("serverCertificateHashes unknown algorithm");
                                serverCertificateHashes.push_back(curhash);
                            }
                        }
                        else
                            return Nan::ThrowError("serverCertificateHashes is not an array");
                    }

                    if (Nan::HasOwnProperty(lobj, localPortProp).FromJust() && !Nan::Get(lobj, localPortProp).IsEmpty())
                    {
                        v8::Local<v8::Value> localPortValue = Nan::Get(lobj, localPortProp).ToLocalChecked();
                        if (localPortValue->IsNumber())
                            local_port = Nan::To<int>(localPortValue).FromJust();
                        else
                            return Nan::ThrowError("localPort is not a number");
                    }
                }
            }

            // Callback *callback, int port, std::unique_ptr<ProofSource> proof_source,  const char *secret

            Http3EventLoop *eventloop = nullptr;
            if (!info[1]->IsUndefined())
            {
                v8::MaybeLocal<v8::Object> obj = info[1]->ToObject(context);
                v8::Local<v8::Object> lobj = obj.ToLocalChecked();
                eventloop = Nan::ObjectWrap::Unwrap<Http3EventLoop>(lobj);
            }
            else
            {
                return Nan::ThrowError("No eventloop arguments passed to Http3Client");
            }

            std::unique_ptr<QuicConnectionHelperInterface> helper =
                std::make_unique<QuicEpollConnectionHelper>(eventloop->getEpollServer(), QuicAllocator::SIMPLE);

            std::unique_ptr<ChromiumWebTransportFingerprintProofVerifier> verifier;

            if (serverCertificateHashes.size() > 0)
            {
                verifier = std::make_unique<ChromiumWebTransportFingerprintProofVerifier>(helper->GetClock(), 14);

                for (auto cur = serverCertificateHashes.begin();
                     cur != serverCertificateHashes.end(); cur++)
                {
                    if (!verifier->AddFingerprint(*cur))
                    {
                        return Nan::ThrowError("serverCertificateHashes is not valid fingerprint");
                    }
                }
            }
            else
                return Nan::ThrowError("No supported verification method included");

            std::unique_ptr<Http3SessionCache> cache;

            /* Http3Client(Http3EventLoop *eventloop, QuicSocketAddress server_address,
                const std::string &server_hostname,
                std::unique_ptr<ProofVerifier> proof_verifier,
                std::unique_ptr<SessionCache> session_cache);*/

            QuicSocketAddress address;

            addrinfo hint;
            memset(&hint, 0, sizeof(hint));
            hint.ai_family = AF_INET6;
            hint.ai_protocol = IPPROTO_UDP;

            addrinfo *info_list = nullptr;
            int result = getaddrinfo(hostname.c_str(), port.c_str(), &hint, &info_list);
            if (result != 0)
            {
                QUIC_LOG(ERROR) << "Failed to look up via ipv6" << hostname << ": "
                                << gai_strerror(result);
                info_list = nullptr;
                hint.ai_family = AF_INET;
                int result = getaddrinfo(hostname.c_str(), port.c_str(), &hint, &info_list);
                if (result != 0)
                {
                    QUIC_LOG(ERROR) << "Failed to look up via ipv4" << hostname << ": "
                                    << gai_strerror(result);
                    return Nan::ThrowError("URL host lookup failed");
                }
            }

            QUICHE_CHECK(info_list != nullptr);
            std::unique_ptr<addrinfo, void (*)(addrinfo *)> info_list_owned(info_list,
                                                                            freeaddrinfo);
            address = QuicSocketAddress(info_list->ai_addr, info_list->ai_addrlen);

            Http3Client *object = new Http3Client(eventloop, address, hostname, local_port,
                                                  std::move(verifier), std::move(cache), std::move(helper));
            object->SetUserAgentID("fails-components/webtransport");
            object->Wrap(info.This());
            info.GetReturnValue().Set(info.This());

            object->Ref(); // do not garbage collect

            std::function<void()> task = [object]()
            {
                object->Connect();
            };
            object->eventloop_->Schedule(task);
        }
        else
        {
            const int argc = 2;
            v8::Local<v8::Value> argv[argc] = {info[0], info[1]};
            v8::Local<v8::Function> cons = Nan::New(constructor());
            auto instance = Nan::NewInstance(cons, argc, argv);
            if (!instance.IsEmpty())
                info.GetReturnValue().Set(instance.ToLocalChecked());
        }
    }
    NAN_METHOD(Http3Client::openWTSession)
    {
        Http3Client *obj = Nan::ObjectWrap::Unwrap<Http3Client>(info.Holder());
        // got the object we can now start the server

        v8::Isolate *isolate = info.GetIsolate();
        v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();

        if (!info[0]->IsUndefined())
        {
            std::string lpath(*v8::String::Utf8Value(isolate, info[0]->ToString(context).ToLocalChecked()));
            std::function<void()> task = [obj, lpath]()
            {
                obj->openWTSessionInt(lpath);
            };
            obj->eventloop_->Schedule(task);
        }
        else
            return Nan::ThrowError("openWTSession without path");
    }

    NAN_METHOD(Http3Client::closeClient)
    {
        Http3Client *obj = Nan::ObjectWrap::Unwrap<Http3Client>(info.Holder());
        // got the object we can now start the server
        std::function<void()> task = [obj]()
        {
            if (!obj->closeClientInt())
            {
                return Nan::ThrowError("closeClientInt failed for Http3Client");
            }
        };
        obj->eventloop_->Schedule(task);
    }

} // namespace quic
