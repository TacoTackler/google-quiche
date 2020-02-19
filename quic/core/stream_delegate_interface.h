// Copyright (c) 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef QUICHE_QUIC_CORE_STREAM_DELEGATE_INTERFACE_H_
#define QUICHE_QUIC_CORE_STREAM_DELEGATE_INTERFACE_H_

#include <cstddef>
#include "net/third_party/quiche/src/quic/core/quic_types.h"
#include "net/third_party/quiche/src/spdy/core/spdy_protocol.h"

namespace quic {

class QuicStream;

class QUIC_EXPORT_PRIVATE StreamDelegateInterface {
 public:
  virtual ~StreamDelegateInterface() {}

  // Called when the stream has encountered errors that it can't handle.
  virtual void OnStreamError(QuicErrorCode error_code,
                             std::string error_details) = 0;
  // Called when the stream needs to write data.
  virtual QuicConsumedData WritevData(QuicStream* stream,
                                      QuicStreamId id,
                                      size_t write_length,
                                      QuicStreamOffset offset,
                                      StreamSendingState state) = 0;
  // Called on stream creation.
  virtual void RegisterStreamPriority(
      QuicStreamId id,
      bool is_static,
      const spdy::SpdyStreamPrecedence& precedence) = 0;
  // Called on stream destruction to clear priority.
  virtual void UnregisterStreamPriority(QuicStreamId id, bool is_static) = 0;
  // Called by the stream on SetPriority to update priority.
  virtual void UpdateStreamPriority(
      QuicStreamId id,
      const spdy::SpdyStreamPrecedence& new_precedence) = 0;
};

}  // namespace quic

#endif  // QUICHE_QUIC_CORE_STREAM_DELEGATE_INTERFACE_H_