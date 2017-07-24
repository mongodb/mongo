/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/op_msg.h"

namespace mongo {

class InsertOp {
public:
    static write_ops::Insert parse(const OpMsgRequest& request);
    static write_ops::Insert parseLegacy(const Message& msg);
};

class UpdateOp {
public:
    static write_ops::Update parse(const OpMsgRequest& request);
    static write_ops::Update parseLegacy(const Message& msg);
};

class DeleteOp {
public:
    static write_ops::Delete parse(const OpMsgRequest& request);
    static write_ops::Delete parseLegacy(const Message& msg);
};

namespace write_ops {

// Limit of the number of operations that can be included in a single write command. This is an
// attempt to avoid a large number of errors resulting in a reply that exceeds 16MB. It doesn't
// fully ensure that goal, but it reduces the probability of it happening. This limit should not be
// used if the protocol changes to avoid the 16MB limit on reply size.
const size_t kMaxWriteBatchSize{1000};

/**
 * Retrieves the statement id for the write at the specified position in the write batch entries
 * array.
 */
int32_t getStmtIdForWriteAt(const WriteCommandBase& writeCommandBase, size_t writePos);

template <class T>
int32_t getStmtIdForWriteAt(const T& op, size_t writePos) {
    return getStmtIdForWriteAt(op.getWriteCommandBase(), writePos);
}

/**
 * Must only be called if the insert is for the "system.indexes" namespace. Returns the actual
 * namespace for which the index is being created.
 */
NamespaceString extractIndexedNamespace(const Insert& insertOp);

// TODO: Delete this getter once IDL supports defaults for object and array fields
template <class T>
const BSONObj& collationOf(const T& opEntry) {
    static const BSONObj emptyBSON{};
    return opEntry.getCollation().get_value_or(emptyBSON);
}

// TODO: Delete this getter once IDL supports defaults for object and array fields
template <class T>
const std::vector<BSONObj>& arrayFiltersOf(const T& opEntry) {
    static const std::vector<BSONObj> emptyBSONArray{};
    return opEntry.getArrayFilters().get_value_or(emptyBSONArray);
}

}  // namespace write_ops
}  // namespace mongo
