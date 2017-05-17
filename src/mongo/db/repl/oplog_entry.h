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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"

namespace mongo {
namespace repl {

/**
 * A parsed oplog entry that inherits from the OplogEntryBase parsed by the IDL.
 */
class OplogEntry : public OplogEntryBase {
public:
    enum class CommandType {
        kNotCommand,
        kCreate,
        kRenameCollection,
        kDrop,
        kCollMod,
        kApplyOps,
        kDropDatabase,
        kEmptyCapped,
        kConvertToCapped,
        kCreateIndex,
        kDropIndexes
    };

    // Current oplog version, should be the value of the v field in all oplog entries.
    static const int kOplogVersion;

    static StatusWith<OplogEntry> parse(const BSONObj& object);

    OplogEntry(OpTime opTime,
               long long hash,
               OpTypeEnum opType,
               NamespaceString nss,
               int version,
               const BSONObj& oField,
               const BSONObj& o2Field);
    OplogEntry(OpTime opTime,
               long long hash,
               OpTypeEnum opType,
               NamespaceString nss,
               int version,
               const BSONObj& oField);
    OplogEntry(OpTime opTime,
               long long hash,
               OpTypeEnum opType,
               NamespaceString nss,
               const BSONObj& oField);
    OplogEntry(OpTime opTime,
               long long hash,
               OpTypeEnum opType,
               NamespaceString nss,
               const BSONObj& oField,
               const BSONObj& o2Field);

    // DEPRECATED: This constructor can throw. Use static parse method instead.
    explicit OplogEntry(BSONObj raw);

    OplogEntry() = delete;

    // This member is not parsed from the BSON and is instead populated by fillWriterVectors.
    bool isForCappedCollection = false;

    /**
     * Returns if the oplog entry is for a command operation.
     */
    bool isCommand() const;

    /**
     * Returns if the oplog entry is for a CRUD operation.
     */
    bool isCrudOpType() const;

    /**
     * Returns the _id of the document being modified. Must be called on CRUD ops.
     */
    BSONElement getIdElement() const;

    /**
     * Returns the type of command of the oplog entry. Must be called on a command op.
     */
    CommandType getCommandType() const;

    /**
     * Returns the OpTime of the oplog entry.
     */
    OpTime getOpTime() const;

    /**
     * Serializes the oplog entry to a string.
     */
    std::string toString() const;

    // TODO (SERVER-29200): make `raw` private. Do not add more direct uses of `raw`.
    BSONObj raw;  // Owned.

private:
    CommandType _commandType;
};

std::ostream& operator<<(std::ostream& s, const OplogEntry& o);

inline bool operator==(const OplogEntry& lhs, const OplogEntry& rhs) {
    return SimpleBSONObjComparator::kInstance.evaluate(lhs.raw == rhs.raw);
}

}  // namespace repl
}  // namespace mongo
