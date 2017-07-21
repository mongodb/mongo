/**
 *    Copyright 2017 (C) MongoDB Inc.
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

#include <initializer_list>
#include <ostream>
#include <string>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/sync_tail_test_fixture.h"
#include "mongo/util/duration.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace repl {

struct CollectionState {
    CollectionState() = default;
    CollectionState(CollectionOptions collectionOptions_,
                    BSONObjSet indexSpecs_,
                    std::string dataHash_);

    /**
     * Compares BSON objects (BSONObj) in two sets of BSON objects (BSONObjSet) to see if the two
     * sets are equivalent.
     *
     * Two sets are equivalent if and only if their sizes are the same and all of their elements
     * that share the same index position are also equivalent in value.
     */
    bool cmpIndexSpecs(const BSONObjSet& otherSpecs) const;

    /**
     * Returns a std::string representation of the CollectionState struct of which this is a member
     * function. Returns out its representation in the form:
     *
     * Collection options: {...}; Index options: [...]; MD5 hash: <md5 digest string>
     */
    std::string toString() const;

    const CollectionOptions collectionOptions = CollectionOptions();
    const BSONObjSet indexSpecs = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    const std::string dataHash = "";
    const bool exists = false;
};

bool operator==(const CollectionState& lhs, const CollectionState& rhs);
std::ostream& operator<<(std::ostream& stream, const CollectionState& state);

class IdempotencyTest : public SyncTailTest {
protected:
    OplogEntry createCollection(CollectionUUID uuid = UUID::gen());
    OplogEntry insert(const BSONObj& obj);
    template <class IdType>
    OplogEntry update(IdType _id, const BSONObj& obj);
    OplogEntry buildIndex(const BSONObj& indexSpec, const BSONObj& options = BSONObj());
    OplogEntry dropIndex(const std::string& indexName);
    OpTime nextOpTime() {
        static long long lastSecond = 1;
        return OpTime(Timestamp(Seconds(lastSecond++), 0), 1LL);
    }
    Status runOp(const OplogEntry& entry);
    Status runOps(std::initializer_list<OplogEntry> ops);
    /**
     * This method returns true if running the list of operations a single time is equivalent to
     * running them two times. It returns false otherwise.
     */
    void testOpsAreIdempotent(std::initializer_list<OplogEntry> ops);

    /**
     * Validate data and indexes. Return the MD5 hash of the documents ordered by _id.
     */
    CollectionState validate();

    NamespaceString nss{"test.foo"};
    NamespaceString nssIndex{"test.system.indexes"};
};

OplogEntry makeCreateCollectionOplogEntry(OpTime opTime,
                                          const NamespaceString& nss = NamespaceString("test.t"),
                                          const BSONObj& options = BSONObj());

OplogEntry makeInsertDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToInsert);

OplogEntry makeUpdateDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToUpdate,
                                        const BSONObj& updatedDocument);

OplogEntry makeCreateIndexOplogEntry(OpTime opTime,
                                     const NamespaceString& nss,
                                     const std::string& indexName,
                                     const BSONObj& keyPattern);

OplogEntry makeCommandOplogEntry(OpTime opTime, const NamespaceString& nss, const BSONObj& command);

}  // namespace repl
}  // namespace mongo
