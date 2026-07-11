// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <initializer_list>
#include <ostream>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class Collection;
class CollectionPtr;

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
StringBuilder& operator<<(StringBuilder& sb, const CollectionState& state);

class [[MONGO_MOD_OPEN]] IdempotencyTest : public OplogApplierImplTest {
public:
    IdempotencyTest()
        : _nss(NamespaceString::createNamespaceString_forTest(boost::none, "test.foo")) {
        globalFailPointRegistry()
            .find("doUntimestampedWritesForIdempotencyTests")
            ->setMode(FailPoint::alwaysOn);
    }

    ~IdempotencyTest() override {
        globalFailPointRegistry()
            .find("doUntimestampedWritesForIdempotencyTests")
            ->setMode(FailPoint::off);
    }

protected:
    enum class SequenceType : int { kEntireSequence, kAnyPrefix, kAnySuffix, kAnyPrefixOrSuffix };
    OplogEntry createCollection(UUID uuid = UUID::gen());
    OplogEntry insert(const BSONObj& obj);
    template <class IdType>
    OplogEntry update(IdType _id, const BSONObj& obj);
    OplogEntry buildIndex(const BSONObj& indexSpec, const BSONObj& options, const UUID& uuid);
    OplogEntry dropIndex(const std::string& indexName, const UUID& uuid);
    OplogEntry prepare(LogicalSessionId lsid,
                       TxnNumber txnNum,
                       StmtId stmtId,
                       const BSONArray& ops,
                       OpTime prevOpTime = OpTime());
    OplogEntry commitUnprepared(LogicalSessionId lsid,
                                TxnNumber txnNum,
                                StmtId stmtId,
                                const BSONArray& ops,
                                OpTime prevOpTime = OpTime());
    OplogEntry commitPrepared(LogicalSessionId lsid,
                              TxnNumber txnNum,
                              StmtId stmtId,
                              OpTime prepareOpTime);
    OplogEntry abortPrepared(LogicalSessionId lsid,
                             TxnNumber txnNum,
                             StmtId stmtId,
                             OpTime prepareOpTime);
    OplogEntry partialTxn(LogicalSessionId lsid,
                          TxnNumber txnNum,
                          StmtId stmtId,
                          OpTime prevOpTime,
                          const BSONArray& ops);
    virtual Status resetState();

    void setNss(const NamespaceString& nss);
    /**
     * This method returns true if running the list of operations a single time is equivalent to
     * running them two times. It returns false otherwise.
     */
    void testOpsAreIdempotent(std::vector<OplogEntry> ops,
                              SequenceType sequenceType = SequenceType::kEntireSequence);

    /**
     * This function exists to work around the issue described in SERVER-30470 by providing a
     * mechanism for the RandomizedIdempotencyTest class to avoid triggering failures caused by
     * differences in the ordering of fields within a document. By default it returns the document
     * unchanged.
     */
    virtual BSONObj canonicalizeDocumentForDataHash(const BSONObj& obj) {
        return obj;
    };

    std::string computeDataHash(const CollectionAcquisition& collection);
    virtual std::string getStatesString(const std::vector<CollectionState>& state1,
                                        const std::vector<CollectionState>& state2,
                                        const std::vector<OplogEntry>& state1Ops,
                                        const std::vector<OplogEntry>& state2Ops);
    /**
     * Validate data and indexes. Return the MD5 hash of the documents ordered by _id.
     */
    CollectionState validate(const NamespaceString& nss);
    std::vector<CollectionState> validateAllCollections();

    NamespaceString _nss;
};

}  // namespace repl
}  // namespace mongo
