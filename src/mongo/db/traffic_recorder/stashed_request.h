// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/operation_context.h"

#include <boost/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
class StashedRequest {
public:
    static const OperationContext::Decoration<StashedRequest> get;

    void clear();

    void set(const Message& message);

    boost::optional<Message> take();

private:
    boost::optional<Message> _value;
};
}  // namespace mongo
