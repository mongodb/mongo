// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/pipelined_oplog_applier.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <vector>

namespace mongo::repl {
namespace {

std::unique_ptr<PipelinedOplogApplier> makeApplier() {
    return std::make_unique<PipelinedOplogApplier>(
        nullptr /* executor */,
        nullptr /* oplogBuffer */,
        &repl::noopOplogApplierObserver,
        nullptr /* replCoord */,
        nullptr /* storageInterface */,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        16 /* numWorkers */);
}

using PipelinedOplogApplierTest = ServiceContextTest;
using PipelinedOplogApplierDeathTest = ServiceContextTest;

TEST_F(PipelinedOplogApplierTest, ConstructsWithSecondaryMode) {
    auto applier = makeApplier();
    ASSERT_EQ(applier->getOptions().mode, repl::OplogApplication::Mode::kSecondary);
}

DEATH_TEST_REGEX_F(PipelinedOplogApplierDeathTest,
                   ApplyOplogBatchIsUnimplemented,
                   "Hit a MONGO_UNIMPLEMENTED") {
    auto applier = makeApplier();
    applier->applyOplogBatch(nullptr /* opCtx */, {}).getStatus().ignore();
}

// Exercises applyWorkItem() and consumeWorkItem() against real oplog entries.
class PipelinedWorkItemTest : public repl::OplogApplierImplTest {
protected:
    using WorkItem = PipelinedApplierWorkerPool::WorkItem;

    static repl::OplogApplier::Options secondaryOptions() {
        return repl::OplogApplier::Options(OplogApplication::Mode::kSecondary);
    }

    static WorkItem makeItem(std::vector<OplogEntry> ops) {
        WorkItem item;
        item.ops = std::move(ops);
        return item;
    }

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

    long long countDocs(const NamespaceString& nss) {
        DBDirectClient client(_opCtx.get());
        return client.count(nss);
    }

    // Consumes the items on a single production worker thread, in order, and waits for it to
    // finish.
    void consumeOnWorker(const repl::OplogApplier::Options& options, std::vector<WorkItem> items) {
        PipelinedApplierWorkerPool pool(1 /* numWorkers */,
                                        [&](size_t workerIdx, const WorkItem& item) {
                                            consumeWorkItem(workerIdx, options, item);
                                        });
        for (auto& item : items) {
            pool.enqueue(0, std::move(item));
        }
        pool.shutdownAndJoin();
    }

    const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.poa_apply");
};

TEST_F(PipelinedWorkItemTest, AppliesOpsInWorkItemOrder) {
    repl::createCollectionWithUuid(_opCtx.get(), kNss);
    const auto options = secondaryOptions();

    // The final state is only correct if the ops are applied in the order given.
    auto item = makeItem({insertOp(kNss, BSON("_id" << 0)),
                          updateOp(kNss, BSON("_id" << 0), BSON("a" << 1)),
                          updateOp(kNss, BSON("_id" << 0), BSON("a" << 2)),
                          insertOp(kNss, BSON("_id" << 1)),
                          deleteOp(kNss, BSON("_id" << 1))});
    ASSERT_OK(applyWorkItem(_opCtx.get(), options, item));

    ASSERT_EQ(countDocs(kNss), 1);
    ASSERT_TRUE(repl::docExists(_opCtx.get(), kNss, BSON("_id" << 0 << "a" << 2)));
}

TEST_F(PipelinedWorkItemTest, GroupsConsecutiveInsertsOnTheSameCollection) {
    repl::createCollectionWithUuid(_opCtx.get(), kNss);
    const auto options = secondaryOptions();

    std::vector<size_t> insertBatchSizes;
    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString&, const std::vector<BSONObj>& docs) {
            insertBatchSizes.push_back(docs.size());
        };

    auto item = makeItem({insertOp(kNss, BSON("_id" << 0)),
                          insertOp(kNss, BSON("_id" << 1)),
                          insertOp(kNss, BSON("_id" << 2))});
    ASSERT_OK(applyWorkItem(_opCtx.get(), options, item));

    // Exactly one insert call, carrying all three documents: the shared apply path groups
    // consecutive inserts to one collection.
    ASSERT_EQ(insertBatchSizes, (std::vector<size_t>{3}));
    ASSERT_EQ(countDocs(kNss), 3);
}

TEST_F(PipelinedWorkItemTest, RetriesWriteConflicts) {
    repl::createCollectionWithUuid(_opCtx.get(), kNss);
    const auto options = secondaryOptions();

    int insertAttempts = 0;
    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString&, const std::vector<BSONObj>&) {
            if (++insertAttempts == 1) {
                throwWriteConflictException("injected write conflict");
            }
        };

    ASSERT_OK(applyWorkItem(_opCtx.get(), options, makeItem({insertOp(kNss, BSON("_id" << 0))})));

    ASSERT_EQ(insertAttempts, 2);
    ASSERT_TRUE(repl::docExists(_opCtx.get(), kNss, BSON("_id" << 0)));
}

TEST_F(PipelinedWorkItemTest, ReturnsErrorWhenCollectionIsMissingInSteadyState) {
    repl::createDatabase(_opCtx.get(), kNss.db_forTest());
    const auto options = secondaryOptions();

    auto status =
        applyWorkItem(_opCtx.get(), options, makeItem({insertOp(kNss, BSON("_id" << 0))}));

    ASSERT_EQ(status, ErrorCodes::NamespaceNotFound);
    ASSERT_FALSE(repl::collectionExists(_opCtx.get(), kNss));
}

TEST_F(PipelinedWorkItemTest, IgnoresMissingCollectionWhenOptionsAllowIt) {
    repl::createDatabase(_opCtx.get(), kNss.db_forTest());
    // kInitialSync is the mode whose Options set allowNamespaceNotFoundErrorsOnCrudOps.
    const repl::OplogApplier::Options options(OplogApplication::Mode::kInitialSync);

    ASSERT_OK(applyWorkItem(_opCtx.get(), options, makeItem({insertOp(kNss, BSON("_id" << 0))})));
    ASSERT_FALSE(repl::collectionExists(_opCtx.get(), kNss));
}

TEST_F(PipelinedWorkItemTest, AppliesCommands) {
    const auto options = secondaryOptions();
    auto createOp = repl::makeCreateCollectionOplogEntry(_opCtx.get(), nextOpTime(), kNss);

    ASSERT_OK(applyWorkItem(
        _opCtx.get(), options, makeItem({createOp, insertOp(kNss, BSON("_id" << 0))})));

    ASSERT_TRUE(repl::collectionExists(_opCtx.get(), kNss));
    ASSERT_TRUE(repl::docExists(_opCtx.get(), kNss, BSON("_id" << 0)));
}

TEST_F(PipelinedWorkItemTest, FallsBackToSingleInsertsWhenAnInjectedGroupedInsertFails) {
    repl::createCollectionWithUuid(_opCtx.get(), kNss);
    const auto options = secondaryOptions();

    // Fail any insert carrying more than one document. This stands in for a grouped insert
    // hitting an error such as a duplicate key, after which the shared apply path applies the
    // ops one at a time so the failing op is isolated.
    std::vector<size_t> insertBatchSizes;
    _opObserver->onInsertsFn = [&](OperationContext*,
                                   const NamespaceString&,
                                   const std::vector<BSONObj>& docs) {
        insertBatchSizes.push_back(docs.size());
        uassert(ErrorCodes::OperationFailed, "injected grouped insert failure", docs.size() == 1);
    };

    ASSERT_OK(applyWorkItem(_opCtx.get(),
                            options,
                            makeItem({insertOp(kNss, BSON("_id" << 0)),
                                      insertOp(kNss, BSON("_id" << 1)),
                                      insertOp(kNss, BSON("_id" << 2))})));

    // The grouped attempt, then each op individually.
    ASSERT_EQ(insertBatchSizes, (std::vector<size_t>{3, 1, 1, 1}));
    ASSERT_EQ(countDocs(kNss), 3);
}

TEST_F(PipelinedWorkItemTest, SetsWorkerOpCtxStates) {
    repl::createCollectionWithUuid(_opCtx.get(), kNss);
    const auto options = secondaryOptions();

    // Recorded rather than asserted inside the hook: applyOplogBatchCommon is noexcept, so a
    // throwing ASSERT would terminate the test instead of failing it.
    bool observed = false;
    bool writesReplicated = true;
    bool enforcingConstraints = true;
    auto prepareConflictBehavior = PrepareConflictBehavior::kEnforce;
    auto admissionPriority = AdmissionContext::Priority::kNormal;
    _opObserver->onInsertsFn =
        [&](OperationContext* opCtx, const NamespaceString&, const std::vector<BSONObj>&) {
            observed = true;
            writesReplicated = opCtx->writesAreReplicated();
            enforcingConstraints = opCtx->isEnforcingConstraints();
            prepareConflictBehavior =
                shard_role_details::getRecoveryUnit(opCtx)->getPrepareConflictBehavior();
            admissionPriority = ExecutionAdmissionContext::get(opCtx).getPriority();
        };

    ASSERT_OK(applyWorkItem(_opCtx.get(), options, makeItem({insertOp(kNss, BSON("_id" << 0))})));

    ASSERT_TRUE(observed);
    ASSERT_FALSE(writesReplicated);
    ASSERT_FALSE(enforcingConstraints);
    ASSERT_EQ(prepareConflictBehavior, PrepareConflictBehavior::kIgnoreConflictsAllowWrites);
    ASSERT_EQ(admissionPriority, AdmissionContext::Priority::kExempt);
}

TEST_F(PipelinedWorkItemTest, WorkerAppliesItemsInFifoOrder) {
    repl::createCollectionWithUuid(_opCtx.get(), kNss);

    // Each item depends on the one before it, so the final state is only correct in FIFO order.
    std::vector<WorkItem> items;
    items.push_back(makeItem({insertOp(kNss, BSON("_id" << 0))}));
    items.push_back(makeItem({updateOp(kNss, BSON("_id" << 0), BSON("a" << 1))}));
    items.push_back(makeItem({insertOp(kNss, BSON("_id" << 1))}));
    items.push_back(makeItem({deleteOp(kNss, BSON("_id" << 1))}));
    consumeOnWorker(secondaryOptions(), std::move(items));

    ASSERT_EQ(countDocs(kNss), 1);
    ASSERT_TRUE(repl::docExists(_opCtx.get(), kNss, BSON("_id" << 0 << "a" << 1)));
}

using PipelinedWorkItemDeathTest = PipelinedWorkItemTest;

DEATH_TEST_REGEX_F(PipelinedWorkItemDeathTest, ConsumeRejectsEmptyWorkItem, "item.ops.empty") {
    std::vector<WorkItem> items(1);
    consumeOnWorker(secondaryOptions(), std::move(items));
}

DEATH_TEST_REGEX_F(PipelinedWorkItemDeathTest,
                   ConsumeFassertsOnUnrecoverableError,
                   "13322100.*Pipelined oplog applier worker failed to apply a work item") {
    repl::createCollectionWithUuid(_opCtx.get(), kNss);
    const auto options = secondaryOptions();

    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString&, const std::vector<BSONObj>&) {
            uasserted(ErrorCodes::OperationFailed, "injected unrecoverable failure");
        };

    std::vector<WorkItem> items;
    items.push_back(makeItem({insertOp(kNss, BSON("_id" << 0))}));
    consumeOnWorker(options, std::move(items));
}

}  // namespace
}  // namespace mongo::repl
