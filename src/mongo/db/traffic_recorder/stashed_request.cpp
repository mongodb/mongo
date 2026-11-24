// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/traffic_recorder/stashed_request.h"

#include "mongo/base/error_codes.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"

#include <cstring>

namespace mongo {

const OperationContext::Decoration<StashedRequest> StashedRequest::get =
    OperationContext::declareDecoration<StashedRequest>();

void StashedRequest::clear() {
    _value.reset();
}

void StashedRequest::set(const Message& message) {
    if (message.empty()) {
        _value = message;
        return;
    }
    uassert(ErrorCodes::BadValue,
            "Invalid message size for traffic recording",
            message.capacity() >= MsgData::MsgDataHeaderSize &&
                message.size() >= MsgData::MsgDataHeaderSize &&
                static_cast<size_t>(message.size()) <= message.capacity());
    auto buffer = SharedBuffer::allocate(message.size());
    std::memcpy(buffer.get(), message.buf(), message.size());
    _value = Message(std::move(buffer));
}

boost::optional<Message> StashedRequest::take() {
    return std::move(_value);
}
}  // namespace mongo
