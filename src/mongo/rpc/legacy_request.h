// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"

namespace mongo {
namespace rpc {

OpMsgRequest opMsgRequestFromLegacyRequest(const Message& message);

}  // namespace rpc
}  // namespace mongo
