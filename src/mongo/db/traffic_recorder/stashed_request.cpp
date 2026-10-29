// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/traffic_recorder/stashed_request.h"

#include "mongo/transport/session.h"

namespace mongo {

const OperationContext::Decoration<StashedRequest> StashedRequest::get =
    OperationContext::declareDecoration<StashedRequest>();

void StashedRequest::clear() {
    _value.reset();
}

void StashedRequest::set(const Message& message) {
    _value = message;
}

boost::optional<Message> StashedRequest::take() {
    return std::move(_value);
}
}  // namespace mongo
