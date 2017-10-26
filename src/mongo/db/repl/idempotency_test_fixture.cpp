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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/idempotency_test_fixture.h"

#include <string>
#include <utility>
#include <vector>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/util/md5.hpp"

namespace mongo {
namespace repl {

namespace {

/**
 * Creates an OplogEntry with given parameters and preset defaults for this test suite.
 */
repl::OplogEntry makeOplogEntry(repl::OpTime opTime,
                                repl::OpTypeEnum opType,
                                NamespaceString nss,
                                BSONObj object,
                                boost::optional<BSONObj> object2 = boost::none,
                                OperationSessionInfo sessionInfo = {},
                                boost::optional<Date_t> wallClockTime = boost::none,
                                boost::optional<StmtId> stmtId = boost::none) {
    return repl::OplogEntry(opTime,                           // optime
                            1LL,                              // hash
                            opType,                           // opType
                            nss,                              // namespace
                            boost::none,                      // uuid
                            boost::none,                      // fromMigrate
                            repl::OplogEntry::kOplogVersion,  // version
                            object,                           // o
                            object2,                          // o2
                            sessionInfo,                      // sessionInfo
                            wallClockTime,                    // wall clock time
                            stmtId,                           // statement id
                            boost::none,   // optime of previous write within same transaction
                            boost::none,   // pre-image optime
                            boost::none);  // post-image optime
}

}  // namespace

/**
 * Compares BSON objects (BSONObj) in two sets of BSON objects (BSONObjSet) to see if the two
 * sets are equivalent.
 *
 * Two sets are equivalent if and only if their sizes are the same and all of their elements
 * that share the same index position are also equivalent in value.
 */
bool CollectionState::cmpIndexSpecs(const BSONObjSet& otherSpecs) const {
    if (indexSpecs.size() != otherSpecs.size()) {
        return false;
    }

    auto thisIt = this->indexSpecs.begin();
    auto otherIt = otherSpecs.begin();

    // thisIt and otherIt cannot possibly be out of sync in terms of progression through
    // their respective sets because we ensured earlier that their sizes are equal and we
    // increment both by 1 on each iteration. We can avoid checking both iterator positions and
    // only check one (thisIt).
    for (; thisIt != this->indexSpecs.end(); ++thisIt, ++otherIt) {
        // Since these are ordered sets, we expect that in the case of equivalent index specs,
        // each copy will be in the same order in both sets, therefore each loop step should be
        // true.

        if (!thisIt->binaryEqual(*otherIt)) {
            return false;
        }
    }

    return true;
}

/**
 * Returns a std::string representation of the CollectionState struct of which this is a member
 * function. Returns out its representation in the form:
 *
 * Collection options: {...}; Index options: [...]; MD5 hash: <md5 digest string>
 */
std::string CollectionState::toString() const {
    if (!this->exists) {
        return "Collection does not exist.";
    }

    BSONObj collectionOptionsBSON = this->collectionOptions.toBSON();
    StringBuilder sb;
    sb << "Collection options: " << collectionOptionsBSON.toString() << "; ";

    sb << "Index specs: [ ";
    bool firstIter = true;
    for (auto indexSpec : this->indexSpecs) {
        if (!firstIter) {
            sb << ", ";
        } else {
            firstIter = false;
        }
        sb << indexSpec.toString();
    }
    sb << " ]; ";

    sb << "MD5 Hash: ";
    // Be more explicit about CollectionState structs without a supplied MD5 hash string.
    sb << (this->dataHash.length() != 0 ? this->dataHash : "No hash");
    return sb.str();
}

CollectionState::CollectionState(CollectionOptions collectionOptions_,
                                 BSONObjSet indexSpecs_,
                                 std::string dataHash_)
    : collectionOptions(std::move(collectionOptions_)),
      indexSpecs(std::move(indexSpecs_)),
      dataHash(std::move(dataHash_)),
      exists(true){};

bool operator==(const CollectionState& lhs, const CollectionState& rhs) {
    if (!lhs.exists || !rhs.exists) {
        return lhs.exists == rhs.exists;
    }

    BSONObj lhsCollectionOptionsBSON = lhs.collectionOptions.toBSON();
    BSONObj rhsCollectionOptionsBSON = rhs.collectionOptions.toBSON();
    // Since collection options uses deferred comparison, we opt to binary compare its BSON
    // representations.
    bool collectionOptionsEqual = lhsCollectionOptionsBSON.binaryEqual(rhsCollectionOptionsBSON);
    bool indexSpecsEqual = lhs.cmpIndexSpecs(rhs.indexSpecs);
    bool dataHashEqual = lhs.dataHash == rhs.dataHash;
    bool existsEqual = lhs.exists == rhs.exists;
    return collectionOptionsEqual && indexSpecsEqual && dataHashEqual && existsEqual;
}

bool operator!=(const CollectionState& lhs, const CollectionState& rhs) {
    return !(lhs == rhs);
}

std::ostream& operator<<(std::ostream& stream, const CollectionState& state) {
    return stream << state.toString();
}

StringBuilderImpl<SharedBufferAllocator>& operator<<(StringBuilderImpl<SharedBufferAllocator>& sb,
                                                     const CollectionState& state) {
    return sb << state.toString();
}

const auto kCollectionDoesNotExist = CollectionState();

/**
 * Creates a command oplog entry with given optime and namespace.
 */
OplogEntry makeCommandOplogEntry(OpTime opTime,
                                 const NamespaceString& nss,
                                 const BSONObj& command) {
    return makeOplogEntry(opTime, OpTypeEnum::kCommand, nss.getCommandNS(), command);
}

/**
 * Creates a create collection oplog entry with given optime.
 */
OplogEntry makeCreateCollectionOplogEntry(OpTime opTime,
                                          const NamespaceString& nss,
                                          const BSONObj& options) {
    BSONObjBuilder bob;
    bob.append("create", nss.coll());
    bob.appendElements(options);
    return makeCommandOplogEntry(opTime, nss, bob.obj());
}

/**
 * Creates an insert oplog entry with given optime and namespace.
 */
OplogEntry makeInsertDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToInsert) {
    return makeOplogEntry(opTime,               // optime
                          OpTypeEnum::kInsert,  // op type
                          nss,                  // namespace
                          documentToInsert,     // o
                          boost::none,          // o2
                          {},                   // session info
                          Date_t::now());       // wall clock time
}

/**
 * Creates a delete oplog entry with given optime and namespace.
 */
OplogEntry makeDeleteDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToDelete) {
    return makeOplogEntry(opTime,               // optime
                          OpTypeEnum::kDelete,  // op type
                          nss,                  // namespace
                          documentToDelete,     // o
                          boost::none,          // o2
                          {},                   // session info
                          Date_t::now());       // wall clock time
}

/**
 * Creates an update oplog entry with given optime and namespace.
 */
OplogEntry makeUpdateDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToUpdate,
                                        const BSONObj& updatedDocument) {
    return makeOplogEntry(opTime,               // optime
                          OpTypeEnum::kUpdate,  // op type
                          nss,                  // namespace
                          updatedDocument,      // o
                          documentToUpdate,     // o2
                          {},                   // session info
                          Date_t::now());       // wall clock time
}

/**
 * Creates an index creation entry with given optime and namespace.
 */
OplogEntry makeCreateIndexOplogEntry(OpTime opTime,
                                     const NamespaceString& nss,
                                     const std::string& indexName,
                                     const BSONObj& keyPattern) {
    BSONObjBuilder indexInfoBob;
    indexInfoBob.append("v", 2);
    indexInfoBob.append("key", keyPattern);
    indexInfoBob.append("name", indexName);
    indexInfoBob.append("ns", nss.ns());
    return makeInsertDocumentOplogEntry(
        opTime, NamespaceString(nss.getSystemIndexesCollection()), indexInfoBob.obj());
}

/**
 * Creates an insert oplog entry with given optime, namespace and session info.
 */
OplogEntry makeInsertDocumentOplogEntryWithSessionInfo(OpTime opTime,
                                                       const NamespaceString& nss,
                                                       const BSONObj& documentToInsert,
                                                       OperationSessionInfo info) {
    return makeOplogEntry(opTime,               // optime
                          OpTypeEnum::kInsert,  // op type
                          nss,                  // namespace
                          documentToInsert,     // o
                          boost::none,          // o2
                          info,                 // session info
                          Date_t::now());       // wall clock time
}

OplogEntry makeInsertDocumentOplogEntryWithSessionInfoAndStmtId(OpTime opTime,
                                                                const NamespaceString& nss,
                                                                const BSONObj& documentToInsert,
                                                                LogicalSessionId lsid,
                                                                TxnNumber txnNum,
                                                                StmtId stmtId) {
    OperationSessionInfo info;
    info.setSessionId(lsid);
    info.setTxnNumber(txnNum);
    return makeOplogEntry(opTime,               // optime
                          OpTypeEnum::kInsert,  // op type
                          nss,                  // namespace
                          documentToInsert,     // o
                          boost::none,          // o2
                          info,                 // session info
                          Date_t::now(),        // wall clock time
                          stmtId);              // statement id
}


Status IdempotencyTest::resetState() {
    return Status::OK();
}

void IdempotencyTest::testOpsAreIdempotent(std::vector<OplogEntry> ops, SequenceType sequenceType) {
    ASSERT_OK(resetState());
    ASSERT_OK(runOpsInitialSync(ops));
    auto state1 = validate();
    auto iterations = sequenceType == SequenceType::kEntireSequence ? 1 : ops.size();

    for (std::size_t i = 0; i < iterations; i++) {
        ASSERT_OK(resetState());
        std::vector<OplogEntry> fullSequence;

        if (sequenceType == SequenceType::kEntireSequence) {
            ASSERT_OK(runOpsInitialSync(ops));
            fullSequence.insert(fullSequence.end(), ops.begin(), ops.end());
        } else if (sequenceType == SequenceType::kAnyPrefix ||
                   sequenceType == SequenceType::kAnyPrefixOrSuffix) {
            std::vector<OplogEntry> prefix(ops.begin(), ops.begin() + i + 1);
            ASSERT_OK(runOpsInitialSync(prefix));
            fullSequence.insert(fullSequence.end(), prefix.begin(), prefix.end());
        }

        ASSERT_OK(runOpsInitialSync(ops));
        fullSequence.insert(fullSequence.end(), ops.begin(), ops.end());

        if (sequenceType == SequenceType::kAnySuffix ||
            sequenceType == SequenceType::kAnyPrefixOrSuffix) {
            std::vector<OplogEntry> suffix(ops.begin() + i, ops.end());
            ASSERT_OK(runOpsInitialSync(suffix));
            fullSequence.insert(fullSequence.end(), suffix.begin(), suffix.end());
        }

        auto state2 = validate();
        if (state1 != state2) {
            FAIL(getStateString(state1, state2, fullSequence));
        }
    }
}

OplogEntry IdempotencyTest::createCollection(CollectionUUID uuid) {
    return makeCreateCollectionOplogEntry(nextOpTime(), nss, BSON("uuid" << uuid));
}

OplogEntry IdempotencyTest::dropCollection() {
    return makeCommandOplogEntry(nextOpTime(), nss, BSON("drop" << nss.coll()));
}

OplogEntry IdempotencyTest::insert(const BSONObj& obj) {
    return makeInsertDocumentOplogEntry(nextOpTime(), nss, obj);
}

template <class IdType>
OplogEntry IdempotencyTest::update(IdType _id, const BSONObj& obj) {
    return makeUpdateDocumentOplogEntry(nextOpTime(), nss, BSON("_id" << _id), obj);
}

OplogEntry IdempotencyTest::buildIndex(const BSONObj& indexSpec, const BSONObj& options) {
    BSONObjBuilder bob;
    bob.append("v", 2);
    bob.append("key", indexSpec);
    bob.append("name", std::string(indexSpec.firstElementFieldName()) + "_index");
    bob.append("ns", nss.ns());
    bob.appendElementsUnique(options);
    return makeInsertDocumentOplogEntry(nextOpTime(), nssIndex, bob.obj());
}

OplogEntry IdempotencyTest::dropIndex(const std::string& indexName) {
    auto cmd = BSON("deleteIndexes" << nss.coll() << "index" << indexName);
    return makeCommandOplogEntry(nextOpTime(), nss, cmd);
}

std::string IdempotencyTest::computeDataHash(Collection* collection) {
    IndexDescriptor* desc = collection->getIndexCatalog()->findIdIndex(_opCtx.get());
    ASSERT_TRUE(desc);
    auto exec = InternalPlanner::indexScan(_opCtx.get(),
                                           collection,
                                           desc,
                                           BSONObj(),
                                           BSONObj(),
                                           BoundInclusion::kIncludeStartKeyOnly,
                                           PlanExecutor::NO_YIELD,
                                           InternalPlanner::FORWARD,
                                           InternalPlanner::IXSCAN_FETCH);
    ASSERT(NULL != exec.get());
    md5_state_t st;
    md5_init(&st);

    PlanExecutor::ExecState state;
    BSONObj obj;
    while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, NULL))) {
        obj = this->canonicalizeDocumentForDataHash(obj);
        md5_append(&st, (const md5_byte_t*)obj.objdata(), obj.objsize());
    }
    ASSERT_EQUALS(PlanExecutor::IS_EOF, state);
    md5digest d;
    md5_finish(&st, d);
    return digestToString(d);
}

CollectionState IdempotencyTest::validate() {
    AutoGetCollectionForReadCommand autoColl(_opCtx.get(), nss);
    auto collection = autoColl.getCollection();

    if (!collection) {
        // Return a mostly default initialized CollectionState struct with exists set to false to
        // indicate an unfound Collection (or a view).
        return kCollectionDoesNotExist;
    }
    ValidateResults validateResults;
    BSONObjBuilder bob;

    Lock::DBLock lk(_opCtx.get(), nss.db(), MODE_IX);
    auto lock = stdx::make_unique<Lock::CollectionLock>(_opCtx->lockState(), nss.ns(), MODE_X);
    ASSERT_OK(collection->validate(
        _opCtx.get(), kValidateFull, false, std::move(lock), &validateResults, &bob));
    ASSERT_TRUE(validateResults.valid);

    std::string dataHash = computeDataHash(collection);

    auto collectionCatalog = collection->getCatalogEntry();
    auto collectionOptions = collectionCatalog->getCollectionOptions(_opCtx.get());
    std::vector<std::string> allIndexes;
    BSONObjSet indexSpecs = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    collectionCatalog->getAllIndexes(_opCtx.get(), &allIndexes);
    for (auto const& index : allIndexes) {
        indexSpecs.insert(collectionCatalog->getIndexSpec(_opCtx.get(), index));
    }
    ASSERT_EQUALS(indexSpecs.size(), allIndexes.size());

    CollectionState collectionState(collectionOptions, indexSpecs, dataHash);

    return collectionState;
}

std::string IdempotencyTest::getStateString(const CollectionState& state1,
                                            const CollectionState& state2,
                                            const std::vector<OplogEntry>& ops) {
    StringBuilder sb;
    sb << "The state: " << state1 << " does not match with the state: " << state2
       << " found after applying the operations a second time, therefore breaking idempotency.";
    return sb.str();
}

template OplogEntry IdempotencyTest::update<int>(int _id, const BSONObj& obj);
template OplogEntry IdempotencyTest::update<const char*>(char const* _id, const BSONObj& obj);

}  // namespace repl
}  // namespace mongo
