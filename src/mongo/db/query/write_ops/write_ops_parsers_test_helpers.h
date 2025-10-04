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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/rpc/op_msg.h"

namespace mongo {
/**
 * Returns an OpMsgRequest for the supplied db and cmd. If useDocSequence is true, it will move the
 * following fields from the body to a document sequence:
 *      "documents", "updates", "deletes", "GARBAGE"
 *
 * This is intended to be used like this:
 *
 * const auto cmdObj = BSON( ... );
 * for (auto docSeq : {false, true}) {
 *     auto req = parse(toOpMsg("test", cmdObj, docSeq));
 *     ASSERT(...);
 * }
 */
OpMsgRequest toOpMsg(StringData db, const BSONObj& cmd, bool useDocSequence);

}  // namespace mongo
