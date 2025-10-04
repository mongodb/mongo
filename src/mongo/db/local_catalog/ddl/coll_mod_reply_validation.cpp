/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/local_catalog/ddl/coll_mod_reply_validation.h"

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
