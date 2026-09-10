// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/pipelined_op_router.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_applier_utils.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_or_grouped_inserts.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <set>
#include <vector>

namespace mongo::repl {
namespace {

using OpClass = PipelinedOpRouter::OpClass;

class PipelinedOpRouterTest : public repl::OplogApplierImplTest {
protected:
    static constexpr size_t kNumWorkers = 16;

    OplogEntry insertOp(const NamespaceString& nss, const BSONObj& doc) {
        return repl::makeInsertDocumentOplogEntry(nextOpTime(), nss, doc);
    }

    OplogEntry updateOp(const NamespaceString& nss, const BSONObj& query, const BSONObj& setDoc) {
        return repl::makeUpdateDocumentOplogEntry(
            nextOpTime(),
            nss,
            query,
            update_oplog_entry::makeDeltaOplogEntry(
                BSON(doc_diff::kUpdateSectionFieldName << setDoc)));
    }

    OplogEntry deleteOp(const NamespaceString& nss, const BSONObj& query) {
        return repl::makeDeleteDocumentOplogEntry(nextOpTime(), nss, query);
    }

    OplogEntry noopOp(const NamespaceString& nss, const BSONObj& object) {
        return repl::makeOplogEntry(
            nextOpTime(), OpTypeEnum::kNoop, nss, boost::none /* uuid */, object);
    }

    OplogEntry commandOp(const NamespaceString& nss, const BSONObj& command) {
        return repl::makeCommandOplogEntry(nextOpTime(), nss, command);
    }

    // A non-transactional applyOps entry.
    OplogEntry standaloneApplyOpsOp(std::vector<repl::ReplOperation> ops) {
        return repl::makeApplyOpsOplogEntry(nextOpTime(),
                                            std::move(ops),
                                            OperationSessionInfo(),
                                            Date_t::now(),
                                            {} /* stmtIds */,
                                            boost::none /* prevWriteOpTimeInTransaction */);
    }

    // A transaction applyOps entry. A null 'prevOpTime' marks the transaction's first entry.
    OplogEntry transactionApplyOpsOp(std::vector<repl::ReplOperation> ops,
                                     const OperationSessionInfo& sessionInfo,
                                     repl::OpTime prevOpTime,
                                     repl::ApplyOpsType applyOpsType) {
        return repl::makeApplyOpsOplogEntry(nextOpTime(),
                                            std::move(ops),
                                            sessionInfo,
                                            Date_t::now(),
                                            {} /* stmtIds */,
                                            prevOpTime,
                                            boost::none /* multiOpType */,
                                            applyOpsType);
    }

    repl::ReplOperation innerInsert(const NamespaceString& nss, const BSONObj& doc) {
        return repl::MutableOplogEntry::makeInsertOperation(nss, kUuid, doc, doc);
    }

    OperationSessionInfo makeSessionInfo() {
        OperationSessionInfo sessionInfo;
        sessionInfo.setSessionId(makeLogicalSessionIdForTest());
        sessionInfo.setTxnNumber(1);
        return sessionInfo;
    }

    // Checks the selected worker matches the classic applier's writer index.
    size_t selectWorkerCheckedAgainstClassic(OplogEntry& op) {
        auto worker = _router.selectWorker(_opCtx.get(), &op);
        std::vector<std::vector<repl::ApplierOperation>> writerVectors(kNumWorkers);
        repl::CachedCollectionProperties collPropertiesCache;
        ASSERT_EQ(worker,
                  repl::OplogApplierUtils::addToWriterVector(
                      _opCtx.get(), &op, &writerVectors, &collPropertiesCache));
        ASSERT_LT(worker, kNumWorkers);
        return worker;
    }

    PipelinedOpRouter _router{kNumWorkers};

    const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.poa_route");
    const NamespaceString kOtherNss =
        NamespaceString::createNamespaceString_forTest("test.poa_route_other");
};

TEST_F(PipelinedOpRouterTest, ClassifiesCrudAsPipelined) {
    ASSERT_EQ(_router.classify(insertOp(kNss, BSON("_id" << 0))), OpClass::kPipelined);
    ASSERT_EQ(_router.classify(updateOp(kNss, BSON("_id" << 0), BSON("a" << 1))),
              OpClass::kPipelined);
    ASSERT_EQ(_router.classify(deleteOp(kNss, BSON("_id" << 0))), OpClass::kPipelined);
}

TEST_F(PipelinedOpRouterTest, ClassifiesContainerOpsAsPipelined) {
    auto ident = serviceContext->getStorageEngine()->generateNewInternalIdent();
    auto value = BSONBinData("V", 1, BinDataGeneral);
    ASSERT_EQ(_router.classify(
                  repl::makeContainerInsertOplogEntry(nextOpTime(), ident, 1 /* key */, value)),
              OpClass::kPipelined);
    ASSERT_EQ(_router.classify(
                  repl::makeContainerUpdateOplogEntry(nextOpTime(), ident, 1 /* key */, value)),
              OpClass::kPipelined);
    ASSERT_EQ(
        _router.classify(repl::makeContainerDeleteOplogEntry(nextOpTime(), ident, 1 /* key */)),
        OpClass::kPipelined);
}

TEST_F(PipelinedOpRouterTest, ClassifiesKeyMaterialOpsAsPipelined) {
    ASSERT_EQ(_router.classify(repl::makeOplogEntry(nextOpTime(),
                                                    OpTypeEnum::kKeyMaterial,
                                                    kNss,
                                                    boost::none /* uuid */,
                                                    BSON("keyId" << 1))),
              OpClass::kPipelined);
    ASSERT_EQ(_router.classify(repl::makeOplogEntry(nextOpTime(),
                                                    OpTypeEnum::kCMKRotation,
                                                    kNss,
                                                    boost::none /* uuid */,
                                                    BSON("keyId" << 1))),
              OpClass::kPipelined);
}

TEST_F(PipelinedOpRouterTest, ClassifiesNoopAsPipelined) {
    ASSERT_EQ(_router.classify(noopOp(kNss, BSON("msg" << "periodic noop"))), OpClass::kPipelined);
}

TEST_F(PipelinedOpRouterTest, ClassifiesNewPrimaryNoopAsRequiresInline) {
    // Record ids may be reused across the new-primary noop, so it is applied inline.
    auto op = noopOp(kNss, BSON(repl::kNewPrimaryMsgField << repl::kNewPrimaryMsg));
    ASSERT_TRUE(op.isNewPrimaryNoop());
    ASSERT_EQ(_router.classify(op), OpClass::kRequiresInline);
}

TEST_F(PipelinedOpRouterTest, ClassifiesOpsOnOwnBatchNamespacesAsRequiresInline) {
    // These namespaces get their own batch in the classic applier, for any non-command op type.
    const auto systemViews = NamespaceString::makeSystemDotViewsNamespace(kNss.dbName());
    ASSERT_EQ(_router.classify(insertOp(systemViews, BSON("_id" << "test.v"))),
              OpClass::kRequiresInline);
    ASSERT_EQ(_router.classify(deleteOp(NamespaceString::kServerConfigurationNamespace,
                                        BSON("_id" << "featureCompatibility"))),
              OpClass::kRequiresInline);
    ASSERT_EQ(_router.classify(noopOp(systemViews, BSON("msg" << "noop"))),
              OpClass::kRequiresInline);
}

TEST_F(PipelinedOpRouterTest, ClassifiesCommandsAsRequiresInline) {
    ASSERT_EQ(_router.classify(commandOp(kNss, BSON("create" << kNss.coll()))),
              OpClass::kRequiresInline);
    ASSERT_EQ(_router.classify(commandOp(kNss, BSON("dropDatabase" << 1))),
              OpClass::kRequiresInline);
    ASSERT_EQ(_router.classify(repl::makeCreateIndexOplogEntry(
                  nextOpTime(), kNss, "a_1", BSON("a" << 1), kUuid)),
              OpClass::kRequiresInline);
}

TEST_F(PipelinedOpRouterTest, ClassifiesStandaloneApplyOpsOfWritesAsPipelinedApplyOps) {
    auto op = standaloneApplyOpsOp(
        {innerInsert(kNss, BSON("_id" << 0)), innerInsert(kOtherNss, BSON("_id" << 1))});
    ASSERT_EQ(_router.classify(op), OpClass::kPipelinedApplyOps);
}

TEST_F(PipelinedOpRouterTest, ClassifiesRetryableWriteApplyOpsAsPipelinedApplyOps) {
    // Carries a session and prevOpTime, but is marked as applied separately from any transaction.
    auto op = repl::makeApplyOpsOplogEntry(nextOpTime(),
                                           {innerInsert(kNss, BSON("_id" << 0))},
                                           makeSessionInfo(),
                                           Date_t::now(),
                                           {0} /* stmtIds */,
                                           repl::OpTime() /* prevWriteOpTimeInTransaction */,
                                           repl::MultiOplogEntryType::kApplyOpsAppliedSeparately);
    ASSERT_FALSE(op.applyOpsIsLinkedTransactionally());
    ASSERT_EQ(_router.classify(op), OpClass::kPipelinedApplyOps);
}

TEST_F(PipelinedOpRouterTest, ClassifiesAtomicRetryableWriteApplyOpsAsRequiresInline) {
    // Linked by prevOpTime, so applying it requires a chain walk like a transaction commit.
    auto op = repl::makeApplyOpsOplogEntry(nextOpTime(),
                                           {innerInsert(kNss, BSON("_id" << 0))},
                                           makeSessionInfo(),
                                           Date_t::now(),
                                           {0} /* stmtIds */,
                                           repl::OpTime() /* prevWriteOpTimeInTransaction */,
                                           repl::MultiOplogEntryType::kApplyOpsAppliedAtomically);
    ASSERT_TRUE(op.applyOpsIsLinkedTransactionally());
    ASSERT_EQ(_router.classify(op), OpClass::kRequiresInline);
}

TEST_F(PipelinedOpRouterTest, ClassifiesUnpreparedTransactionEntriesAsRequiresInline) {
    const auto sessionInfo = makeSessionInfo();
    const auto ops = std::vector{innerInsert(kNss, BSON("_id" << 0))};

    auto partial =
        transactionApplyOpsOp(ops, sessionInfo, repl::OpTime(), repl::ApplyOpsType::kPartial);
    ASSERT_TRUE(partial.isPartialTransaction());
    ASSERT_EQ(_router.classify(partial), OpClass::kRequiresInline);

    // The terminal entry of a large transaction, and a single-entry transaction.
    auto commitOfLarge =
        transactionApplyOpsOp(ops, sessionInfo, partial.getOpTime(), repl::ApplyOpsType::kTerminal);
    ASSERT_TRUE(commitOfLarge.applyOpsIsLinkedTransactionally());
    ASSERT_EQ(_router.classify(commitOfLarge), OpClass::kRequiresInline);

    auto singleEntry =
        transactionApplyOpsOp(ops, sessionInfo, repl::OpTime(), repl::ApplyOpsType::kTerminal);
    ASSERT_TRUE(singleEntry.isSingleOplogEntryTransaction());
    ASSERT_EQ(_router.classify(singleEntry), OpClass::kRequiresInline);

    // The abortTransaction a new primary writes for an unprepared transaction it could not finish.
    auto unpreparedAbort =
        repl::makeAbortTransactionOplogEntry(nextOpTime(), sessionInfo, repl::OpTime());
    ASSERT_FALSE(unpreparedAbort.isPreparedAbort());
    ASSERT_EQ(_router.classify(unpreparedAbort), OpClass::kRequiresInline);
}

TEST_F(PipelinedOpRouterTest, ClassifiesPreparedTransactionEntriesAsRequiresInline) {
    const auto sessionInfo = makeSessionInfo();

    auto prepare = transactionApplyOpsOp({innerInsert(kNss, BSON("_id" << 0))},
                                         sessionInfo,
                                         repl::OpTime(),
                                         repl::ApplyOpsType::kPrepare);
    ASSERT_TRUE(prepare.shouldPrepare());
    ASSERT_EQ(_router.classify(prepare), OpClass::kRequiresInline);

    auto commit = repl::makeCommitTransactionOplogEntry(
        nextOpTime(), sessionInfo, prepare.getOpTime().getTimestamp(), prepare.getOpTime());
    ASSERT_TRUE(commit.isPreparedCommit());
    ASSERT_EQ(_router.classify(commit), OpClass::kRequiresInline);

    auto abort =
        repl::makeAbortTransactionOplogEntry(nextOpTime(), sessionInfo, prepare.getOpTime());
    ASSERT_TRUE(abort.isPreparedAbort());
    ASSERT_EQ(_router.classify(abort), OpClass::kRequiresInline);
}

TEST_F(PipelinedOpRouterTest, SelectsTheSameWorkerForOpsOnTheSameDocument) {
    repl::createCollectionWithUuid(_opCtx.get(), kNss);

    auto insert = insertOp(kNss, BSON("_id" << 0));
    auto update = updateOp(kNss, BSON("_id" << 0), BSON("a" << 1));
    auto remove = deleteOp(kNss, BSON("_id" << 0));

    auto worker = selectWorkerCheckedAgainstClassic(insert);
    ASSERT_EQ(selectWorkerCheckedAgainstClassic(update), worker);
    ASSERT_EQ(selectWorkerCheckedAgainstClassic(remove), worker);
}

TEST_F(PipelinedOpRouterTest, SpreadsDocumentsOfOneCollectionAcrossWorkers) {
    repl::createCollectionWithUuid(_opCtx.get(), kNss);

    std::set<size_t> workers;
    for (int id = 0; id < 64; ++id) {
        auto op = insertOp(kNss, BSON("_id" << id));
        workers.insert(selectWorkerCheckedAgainstClassic(op));
    }
    // The hash includes the document key.
    ASSERT_GT(workers.size(), 1U);
}

TEST_F(PipelinedOpRouterTest, SelectsOneWorkerForAllOpsOnACappedCollection) {
    CollectionOptions options;
    options.capped = true;
    options.cappedSize = 1024 * 1024;
    repl::createCollection(_opCtx.get(), kNss, options);

    // Capped collections preserve insertion order, so ops hash on the namespace alone.
    std::set<size_t> workers;
    for (int id = 0; id < 64; ++id) {
        auto op = insertOp(kNss, BSON("_id" << id));
        workers.insert(selectWorkerCheckedAgainstClassic(op));
        ASSERT_TRUE(op.isForCappedCollection());
    }
    ASSERT_EQ(workers.size(), 1U);
}

TEST_F(PipelinedOpRouterTest, SelectsTheSameWorkerForContainerOpsOnTheSameKey) {
    auto ident = serviceContext->getStorageEngine()->generateNewInternalIdent();
    auto value = BSONBinData("V", 1, BinDataGeneral);

    auto insert = repl::makeContainerInsertOplogEntry(nextOpTime(), ident, 1 /* key */, value);
    auto update = repl::makeContainerUpdateOplogEntry(nextOpTime(), ident, 1 /* key */, value);
    auto remove = repl::makeContainerDeleteOplogEntry(nextOpTime(), ident, 1 /* key */);

    auto worker = selectWorkerCheckedAgainstClassic(insert);
    ASSERT_EQ(selectWorkerCheckedAgainstClassic(update), worker);
    ASSERT_EQ(selectWorkerCheckedAgainstClassic(remove), worker);
}

TEST_F(PipelinedOpRouterTest, ClassifyingAnInlineOpRefreshesCollectionProperties) {
    // Cache the properties of a collection that does not exist yet.
    auto before = insertOp(kNss, BSON("_id" << 0));
    _router.selectWorker(_opCtx.get(), &before);
    ASSERT_FALSE(before.isForCappedCollection());

    CollectionOptions options;
    options.capped = true;
    options.cappedSize = 1024 * 1024;
    repl::createCollection(_opCtx.get(), kNss, options);

    // Still served from the cache.
    auto stale = insertOp(kNss, BSON("_id" << 1));
    _router.selectWorker(_opCtx.get(), &stale);
    ASSERT_FALSE(stale.isForCappedCollection());

    ASSERT_EQ(_router.classify(commandOp(kNss, BSON("create" << kNss.coll()))),
              OpClass::kRequiresInline);

    auto fresh = insertOp(kNss, BSON("_id" << 2));
    _router.selectWorker(_opCtx.get(), &fresh);
    ASSERT_TRUE(fresh.isForCappedCollection());
}

TEST_F(PipelinedOpRouterTest, SelectsWorkersWithinRangeForASingleWorker) {
    repl::createCollectionWithUuid(_opCtx.get(), kNss);
    PipelinedOpRouter router(1 /* numWorkers */);

    for (int id = 0; id < 8; ++id) {
        auto op = insertOp(kNss, BSON("_id" << id));
        ASSERT_EQ(router.selectWorker(_opCtx.get(), &op), 0U);
    }
}

using PipelinedOpRouterDeathTest = PipelinedOpRouterTest;

DEATH_TEST_REGEX_F(PipelinedOpRouterDeathTest, RejectsZeroWorkers, "numWorkers > 0") {
    PipelinedOpRouter router(0 /* numWorkers */);
}

}  // namespace
}  // namespace mongo::repl
