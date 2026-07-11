// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string_view>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * This file is included by every IDL-generated command and provides functionality used in
 * serialization and deserialization.
 */

/**
 * Returns true if the provided argument is one that is handled by the command processing layer
 * and should generally be ignored by individual command implementations. In particular,
 * commands that fail on unrecognized arguments must not fail for any of these.
 */
bool isGenericArgument(std::string_view arg);

/**
 * Returns true if the provided reply field is one that is handled by the command processing layer
 * and should generally be ignored by individual command implementations. In particular,
 * replies that fail on unrecognized fields must not fail for any of these.
 */
bool isGenericReply(std::string_view arg);

/**
 * Returns true if arg should be forwarded to shards.
 *
 * See 'CommandHelpers::filterCommandRequestForPassthrough'.
 */
bool shouldForwardToShards(std::string_view arg);

/**
 * Returns true if replyField should be forwarded from shards to clients.
 *
 * See 'CommandHelpers::filterCommandReplyForPassthrough'.
 */
bool shouldForwardFromShards(std::string_view replyField);

}  // namespace mongo
