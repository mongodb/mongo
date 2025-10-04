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

#include "mongo/db/repl/idempotency_test_fixture.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/database_name.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/validate/collection_validation.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/md5.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {
namespace repl {


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
    for (const auto& indexSpec : this->indexSpecs) {
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
      exists(true) {}

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

std::ostream& operator<<(std::ostream& stream, const CollectionState& state) {
    return stream << state.toString();
}

StringBuilder& operator<<(StringBuilder& sb, const CollectionState& state) {
    return sb << state.toString();
}

const auto kCollectionDoesNotExist = CollectionState();


Status IdempotencyTest::resetState() {
    return Status::OK();
}

void IdempotencyTest::setNss(const NamespaceString& nss) {
    _nss = nss;
}

void IdempotencyTest::testOpsAreIdempotent(std::vector<OplogEntry> ops, SequenceType sequenceType) {
    ASSERT_OK(resetState());

    ASSERT_OK(runOpsInitialSync(ops));

    auto state1 = validateAllCollections();
    auto iterations = sequenceType == SequenceType::kEntireSequence ? 1 : ops.size();
    for (std::size_t i = 0; i < iterations; i++) {
        // Since the end state after each iteration is expected to be the same as the start state,
        // we don't drop and re-create the collections. Dropping and re-creating the collections
        // won't work either because we don't have ways to wait until second-phase drop to
        // completely finish.
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

        auto state2 = validateAllCollections();
        if (state1 != state2) {
            FAIL(getStatesString(state1, state2, ops, fullSequence));
        }
    }
}

OplogEntry IdempotencyTest::createCollection(UUID uuid) {
    return makeCreateCollectionOplogEntry(_opCtx.get(), nextOpTime(), _nss, uuid);
}

OplogEntry IdempotencyTest::insert(const BSONObj& obj) {
    return makeInsertDocumentOplogEntry(nextOpTime(), _nss, obj);
}

template <class IdType>
OplogEntry IdempotencyTest::update(IdType _id, const BSONObj& obj) {
    return makeUpdateDocumentOplogEntry(nextOpTime(), _nss, BSON("_id" << _id), obj);
}

OplogEntry IdempotencyTest::buildIndex(const BSONObj& indexSpec,
                                       const BSONObj& options,
                                       const UUID& uuid) {
    return makeCreateIndexOplogEntry(nextOpTime(),
                                     _nss,
                                     fmt::format("{}_index", indexSpec.firstElementFieldName()),
                                     indexSpec,
                                     uuid,
                                     options);
}

OplogEntry IdempotencyTest::dropIndex(const std::string& indexName, const UUID& uuid) {
    auto cmd = BSON("dropIndexes" << _nss.coll() << "index" << indexName);
    return makeCommandOplogEntry(nextOpTime(), _nss, cmd, boost::none /* object2 */, uuid);
}

OplogEntry IdempotencyTest::prepare(LogicalSessionId lsid,
                                    TxnNumber txnNum,
                                    StmtId stmtId,
                                    const BSONArray& ops,
                                    OpTime prevOpTime) {
    OperationSessionInfo info;
    info.setSessionId(lsid);
    info.setTxnNumber(txnNum);
    return makeCommandOplogEntryWithSessionInfoAndStmtIds(
        nextOpTime(),
        _nss,
        BSON("applyOps" << ops << "prepare" << true),
        lsid,
        txnNum,
        {stmtId},
        prevOpTime);
}

OplogEntry IdempotencyTest::commitUnprepared(LogicalSessionId lsid,
                                             TxnNumber txnNum,
                                             StmtId stmtId,
                                             const BSONArray& ops,
                                             OpTime prevOpTime) {
    OperationSessionInfo info;
    info.setSessionId(lsid);
    info.setTxnNumber(txnNum);
    return makeCommandOplogEntryWithSessionInfoAndStmtIds(
        nextOpTime(), _nss, BSON("applyOps" << ops), lsid, txnNum, {stmtId}, prevOpTime);
}

OplogEntry IdempotencyTest::commitPrepared(LogicalSessionId lsid,
                                           TxnNumber txnNum,
                                           StmtId stmtId,
                                           OpTime prepareOpTime) {
    return makeCommandOplogEntryWithSessionInfoAndStmtIds(
        nextOpTime(),
        _nss,
        BSON("commitTransaction" << 1 << "commitTimestamp" << prepareOpTime.getTimestamp()),
        lsid,
        txnNum,
        {stmtId},
        prepareOpTime);
}

OplogEntry IdempotencyTest::abortPrepared(LogicalSessionId lsid,
                                          TxnNumber txnNum,
                                          StmtId stmtId,
                                          OpTime prepareOpTime) {
    return makeCommandOplogEntryWithSessionInfoAndStmtIds(
        nextOpTime(), _nss, BSON("abortTransaction" << 1), lsid, txnNum, {stmtId}, prepareOpTime);
}

OplogEntry IdempotencyTest::partialTxn(LogicalSessionId lsid,
                                       TxnNumber txnNum,
                                       StmtId stmtId,
                                       OpTime prevOpTime,
                                       const BSONArray& ops) {
    OperationSessionInfo info;
    info.setSessionId(lsid);
    info.setTxnNumber(txnNum);
    return makeCommandOplogEntryWithSessionInfoAndStmtIds(
        nextOpTime(),
        _nss,
        BSON("applyOps" << ops << "partialTxn" << true),
        lsid,
        txnNum,
        {stmtId},
        prevOpTime);
}

std::string IdempotencyTest::computeDataHash(const CollectionAcquisition& collection) {
    auto desc = collection.getCollectionPtr()->getIndexCatalog()->findIdIndex(_opCtx.get());
    ASSERT_TRUE(desc);
    auto exec = InternalPlanner::indexScan(_opCtx.get(),
                                           collection,
                                           desc,
                                           BSONObj(),
                                           BSONObj(),
                                           BoundInclusion::kIncludeStartKeyOnly,
                                           PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                           InternalPlanner::FORWARD,
                                           InternalPlanner::IXSCAN_FETCH);
    ASSERT(nullptr != exec.get());
    md5_state_t st;
    md5_init_state(&st);

    PlanExecutor::ExecState state;
    BSONObj obj;
    while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, nullptr))) {
        obj = this->canonicalizeDocumentForDataHash(obj);
        md5_append(&st, (const md5_byte_t*)obj.objdata(), obj.objsize());
    }
    ASSERT_EQUALS(PlanExecutor::IS_EOF, state);
    md5digest d;
    md5_finish(&st, d);
    return digestToString(d);
}

std::vector<CollectionState> IdempotencyTest::validateAllCollections() {
    std::vector<CollectionState> collStates;
    auto catalog = CollectionCatalog::get(_opCtx.get());
    auto dbNames = catalog->getAllDbNames();
    for (auto& dbName : dbNames) {
        // Skip local database.
        if (!dbName.isLocalDB()) {
            std::vector<NamespaceString> collectionNames;
            {
                Lock::DBLock lk(_opCtx.get(), dbName, MODE_S);
                collectionNames = catalog->getAllCollectionNamesFromDb(_opCtx.get(), dbName);
            }
            for (const auto& nss : collectionNames) {
                collStates.push_back(validate(nss));
            }
        }
    }
    return collStates;
}

CollectionAcquisition getCollectionForRead(OperationContext* opCtx, const NamespaceString& nss) {
    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                     repl::ReadConcernArgs::get(opCtx),
                                     mongo::AcquisitionPrerequisites::kRead),
        MODE_IS);
}

CollectionState IdempotencyTest::validate(const NamespaceString& nss) {
    auto collUUID = [&]() -> boost::optional<UUID> {
        auto coll = getCollectionForRead(_opCtx.get(), _nss);
        return coll.exists() ? boost::make_optional(coll.uuid()) : boost::none;
    }();

    if (collUUID) {
        // Allow in-progress indexes to complete before validating collection contents.
        IndexBuildsCoordinator::get(_opCtx.get())
            ->awaitNoIndexBuildInProgressForCollection(_opCtx.get(), collUUID.value());
    }

    {
        auto collection = getCollectionForRead(_opCtx.get(), _nss);

        if (!collection.exists()) {
            // Return a mostly default initialized CollectionState struct with exists set to false
            // to indicate an unfound Collection (or a view).
            return kCollectionDoesNotExist;
        }
    }

    {
        ValidateResults validateResults;

        ASSERT_OK(
            CollectionValidation::validate(_opCtx.get(),
                                           _nss,
                                           CollectionValidation::ValidationOptions{
                                               CollectionValidation::ValidateMode::kForegroundFull,
                                               CollectionValidation::RepairMode::kNone,
                                               /*logDiagnostics=*/false},
                                           &validateResults));
        ASSERT_TRUE(validateResults.isValid());
    }

    auto collection = getCollectionForRead(_opCtx.get(), _nss);
    const auto& collectionPtr = collection.getCollectionPtr();

    std::string dataHash = computeDataHash(collection);

    auto collectionOptions = collectionPtr->getCollectionOptions();
    std::vector<std::string> allIndexes;
    BSONObjSet indexSpecs = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
    collectionPtr->getAllIndexes(&allIndexes);
    for (auto const& index : allIndexes) {
        indexSpecs.insert(collectionPtr->getIndexSpec(index));
    }
    ASSERT_EQUALS(indexSpecs.size(), allIndexes.size());

    CollectionState collectionState(collectionOptions, indexSpecs, dataHash);

    return collectionState;
}

std::string IdempotencyTest::getStatesString(const std::vector<CollectionState>& state1,
                                             const std::vector<CollectionState>& state2,
                                             const std::vector<OplogEntry>& state1Ops,
                                             const std::vector<OplogEntry>& state2Ops) {
    StringBuilder sb;
    sb << "The states:\n";
    for (const auto& s : state1) {
        sb << s << "\n";
    }
    sb << "do not match with the states:\n";
    for (const auto& s : state2) {
        sb << s << "\n";
    }
    sb << "found after applying the operations a second time, therefore breaking idempotency.\n";
    sb << "Applied ops:\n";
    for (const auto& op : state2Ops) {
        sb << op.toStringForLogging() << "\n";
    }
    return sb.str();
}

template OplogEntry IdempotencyTest::update<int>(int _id, const BSONObj& obj);
template OplogEntry IdempotencyTest::update<const char*>(char const* _id, const BSONObj& obj);

BSONObj makeInsertApplyOpsEntry(const NamespaceString& nss, const UUID& uuid, const BSONObj& doc) {
    return BSON("op" << "i"
                     << "ns" << nss.toString_forTest() << "ui" << uuid << "o" << doc);
}
}  // namespace repl
}  // namespace mongo
