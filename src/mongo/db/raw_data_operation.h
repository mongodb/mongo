/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"

namespace mongo {

constexpr inline auto kRawDataFieldName = "rawData"_sd;

/**
 * Returns a settable boolean indicating whether the given operation context is performing a "raw
 * data" operation. When being run on a collection type that stores its data in a different format
 * from that in which users interact with, a "raw data" operation will operate directly on the
 * format in which it is stored.
 */
bool& isRawDataOperation(OperationContext*);

/**
 * Returns the rewritten command object, replacing the collection name with the one provided.
 */
template <class CommandRequest>
BSONObj rewriteCommandForRawDataOperation(const BSONObj& cmd, StringData coll) {
    BSONObjBuilder builder{cmd.objsize()};
    for (auto&& [fieldName, elem] : cmd) {
        if (fieldName == CommandRequest::kCommandName) {
            builder.append(fieldName, coll);
        } else {
            builder.append(elem);
        }
    }
    return builder.obj();
}

}  // namespace mongo
