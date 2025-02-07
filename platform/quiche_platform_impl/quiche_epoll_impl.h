// Copyright (c) 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A toy server, which listens on a specified address for QUIC traffic and
// handles incoming responses.
//
// Note that this server is intended to verify correctness of the client and is
// in no way expected to be performant.
#ifndef NET_QUICHE_PLATFORM_IMPL_QUIC_EPOLL_IMPL_H_
#define NET_QUICHE_PLATFORM_IMPL_QUIC_EPOLL_IMPL_H_

#include "quiche_platform_impl/simple_libuv_epoll_server.h"

namespace quiche {

using QuicheEpollServerImpl = epoll_server::SimpleLibuvEpollServer;
using QuicheEpollEventImpl = epoll_server::LibuvEpollEvent;
using QuicheEpollAlarmBaseImpl = epoll_server::LibuvEpollAlarm;
using QuicheEpollCallbackInterfaceImpl = epoll_server::LibuvEpollCallbackInterface;

}  // namespace quic

#endif  // NET_QUIC_PLATFORM_IMPL_QUIC_EPOLL_IMPL_H_