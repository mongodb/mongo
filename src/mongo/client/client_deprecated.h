/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#pragma once

#include "mongo/bson/json.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/rpc/message.h"

namespace mongo {
/**
 * WARNING: Do not add new uses of anything in this namespace! This exists only to support code
 * paths that still use an OP_QUERY-derived query representation. Additional callers should not be
 * added because OP_QUERY is no longer supported by the shell or server.
 */
namespace client_deprecated {
/**
 * WARNING: This function exists only to support special code paths that use an OP_QUERY-style query
 * representation (even though the OP_QUERY wire protocol message itself is no longer supported). Do
 * not add new callers.
 *
 * Sets the relevant fields in 'findCommand' based on the 'bsonOptions' object and the 'options' bit
 * vector. 'bsonOptions' is formatted like the query object of an OP_QUERY wire protocol message.
 * Similarly, 'options' is a bit vector which is interpreted like the OP_QUERY flags field.
 */
void initFindFromLegacyOptions(BSONObj bsonOptions, int options, FindCommandRequest* findCommand);
}  // namespace client_deprecated
}  // namespace mongo
