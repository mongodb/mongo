// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC_FOR_TECHNICAL_REASONS]];

namespace mongo {
namespace idl {
/**
 * Parse an IDL-defined command from a command request document.
 * If the request includes apiStrict: true along with any unstable fields, an exception will be
 * thrown.
 */
template <typename T>
[[MONGO_MOD_PUBLIC]] T parseCommandDocument(const BSONObj& cmdObj, const IDLParserContext& ctx) {
    DeserializationContext dctx;
    auto cmd = T::parse(cmdObj, ctx, &dctx);
    if (cmd.getGenericArguments().getApiStrict().value_or(false)) {
        dctx.validateApiStrict();
    }
    return cmd;
}

/**
 * Parse an IDL-defined command from a command request.
 * If the request includes apiStrict: true along with any unstable fields, an exception will be
 * thrown.
 */
template <typename T>
[[MONGO_MOD_PUBLIC]] T parseCommandRequest(const OpMsgRequest& req, const IDLParserContext& ctx) {
    DeserializationContext dctx;
    auto cmd = T::parse(req, ctx, &dctx);
    if (cmd.getGenericArguments().getApiStrict().value_or(false)) {
        dctx.validateApiStrict();
    }
    return cmd;
}

}  // namespace idl
}  // namespace mongo
