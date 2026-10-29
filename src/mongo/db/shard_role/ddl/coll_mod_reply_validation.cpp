// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/ddl/coll_mod_reply_validation.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/optional/optional.hpp>

namespace mongo::coll_mod_reply_validation {
void validateReply(const CollModReply& reply) {
    auto hidden_new = reply.getHidden_new().has_value();
    auto hidden_old = reply.getHidden_old().has_value();

    if ((!hidden_new && hidden_old) || (hidden_new && !hidden_old)) {
        uassert(ErrorCodes::CommandResultSchemaViolation,
                str::stream() << "Invalid CollModReply: Reply should define either both fields "
                              << "(hidden_new and hidden_old) or none of them.",
                false);
    }

    auto prepareUnique_new = reply.getPrepareUnique_new().has_value();
    auto prepareUnique_old = reply.getPrepareUnique_old().has_value();

    if ((!prepareUnique_new && prepareUnique_old) || (prepareUnique_new && !prepareUnique_old)) {
        uassert(ErrorCodes::CommandResultSchemaViolation,
                str::stream() << "Invalid CollModReply: Reply should define either both fields "
                              << "(prepareUnique_new and prepareUnique_old) "
                                 "or none of them.",
                false);
    }
}
}  // namespace mongo::coll_mod_reply_validation
