// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/idl/command_generic_argument.h"

#include "mongo/db/namespace_string.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/idl/idl_parser.h"

#include <string_view>

namespace mongo {

bool isGenericArgument(std::string_view arg) {
    return GenericArguments::hasField(arg) || arg == IDLParserContext::kOpMsgDollarDB;
}

bool isGenericReply(std::string_view arg) {
    return GenericReplyFields::hasField(arg);
}

bool shouldForwardToShards(std::string_view arg) {
    return GenericArguments::shouldForwardToShards(arg) && arg != IDLParserContext::kOpMsgDollarDB;
}

bool shouldForwardFromShards(std::string_view replyField) {
    return GenericReplyFields::shouldForwardFromShards(replyField);
}
}  // namespace mongo
