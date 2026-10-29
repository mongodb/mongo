// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/shard_role/ddl/coll_mod_gen.h"
#include "mongo/util/modules.h"

namespace mongo::coll_mod_reply_validation {

/**
 * CollMod reply object requires extra validation, as the current IDL validation capabilities
 * are not sufficient in this case.
 * It is used to check that reply includes:
 *  - (hidden_new and hidden_old) together or none of them.
 *  - (prepareUnique_new and prepareUnique_old) together or none of them."
 */
[[MONGO_MOD_PARENT_PRIVATE]] void validateReply(const CollModReply& reply);

}  // namespace mongo::coll_mod_reply_validation
