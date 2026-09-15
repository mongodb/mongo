// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/pipelined_oplog_applier.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_applier_batcher_test_fixture.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/log_capture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boost/optional.hpp>

namespace mongo::repl {
namespace {

std::unique_ptr<PipelinedOplogApplier> makeApplier() {
    return std::make_unique<PipelinedOplogApplier>(
        nullptr /* executor */,
        nullptr /* oplogBuffer */,
        &noopOplogApplierObserver,
        nullptr /* replCoord */,
        nullptr /* storageInterface */,
        OplogApplier::Options(OplogApplication::Mode::kSecondary),
        16 /* numWorkers */);
}

using PipelinedOplogApplierTest = ServiceContextTest;
using PipelinedOplogApplierDeathTest = ServiceContextTest;

TEST_F(PipelinedOplogApplierTest, ConstructsWithSecondaryMode) {
    auto applier = makeApplier();
    ASSERT_EQ(applier->getOptions().mode, OplogApplication::Mode::kSecondary);
}

DEATH_TEST_REGEX_F(PipelinedOplogApplierDeathTest,
                   ApplyOplogBatchIsUnimplemented,
                   "Hit a MONGO_UNIMPLEMENTED") {
    auto applier = makeApplier();
    applier->applyOplogBatch(nullptr /* opCtx */, {}).getStatus().ignore();
}

// Exercises applyWorkItem() and consumeWorkItem() against real oplog entries.
class PipelinedWorkItemTest : public OplogApplierImplTest {
protected:
    using WorkItem = PipelinedApplierWorkerPool::WorkItem;

    static OplogApplier::Options secondaryOptions() {
        return OplogApplier::Options(OplogApplication::Mode::kSecondary);
    }

    static WorkItem makeItem(std::vector<OplogEntry> ops) {
        WorkItem item;
        item.ops = std::move(ops);
        return item;
    }

    OplogEntry insertOp(const NamespaceString& nss, const BSONObj& doc) {
        return makeInsertDocumentOplogEntry(nextOpTime(), nss, doc);
    }

    OplogEntry updateOp(const NamespaceString& nss, const BSONObj& query, const BSONObj& setDoc) {
        return makeUpdateDocumentOplogEntry(nextOpTime(),
                                            nss,
                                            query,
                                            update_oplog_entry::makeDeltaOplogEntry(
                                                BSON(doc_diff::kUpdateSectionFieldName << setDoc)));
    }

    OplogEntry deleteOp(const NamespaceString& nss, const BSONObj& query) {
        return makeDeleteDocumentOplogEntry(nextOpTime(), nss, query);
    }

    long long countDocs(const NamespaceString& nss) {
        DBDirectClient client(_opCtx.get());
        return client.count(nss);
    }

    // Consumes the items on a single production worker thread, in order, and waits for it to
    // finish.
    void consumeOnWorker(const OplogApplier::Options& options, std::vector<WorkItem> items) {
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
    createCollectionWithUuid(_opCtx.get(), kNss);
    const auto options = secondaryOptions();

    // The final state is only correct if the ops are applied in the order given.
    auto item = makeItem({insertOp(kNss, BSON("_id" << 0)),
                          updateOp(kNss, BSON("_id" << 0), BSON("a" << 1)),
                          updateOp(kNss, BSON("_id" << 0), BSON("a" << 2)),
                          insertOp(kNss, BSON("_id" << 1)),
                          deleteOp(kNss, BSON("_id" << 1))});
    ASSERT_OK(applyWorkItem(_opCtx.get(), options, item));

    ASSERT_EQ(countDocs(kNss), 1);
    ASSERT_TRUE(docExists(_opCtx.get(), kNss, BSON("_id" << 0 << "a" << 2)));
}

TEST_F(PipelinedWorkItemTest, GroupsConsecutiveInsertsOnTheSameCollection) {
    createCollectionWithUuid(_opCtx.get(), kNss);
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
    createCollectionWithUuid(_opCtx.get(), kNss);
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
    ASSERT_TRUE(docExists(_opCtx.get(), kNss, BSON("_id" << 0)));
}

TEST_F(PipelinedWorkItemTest, ReturnsErrorWhenCollectionIsMissingInSteadyState) {
    createDatabase(_opCtx.get(), kNss.db_forTest());
    const auto options = secondaryOptions();

    auto status =
        applyWorkItem(_opCtx.get(), options, makeItem({insertOp(kNss, BSON("_id" << 0))}));

    ASSERT_EQ(status, ErrorCodes::NamespaceNotFound);
    ASSERT_FALSE(collectionExists(_opCtx.get(), kNss));
}

TEST_F(PipelinedWorkItemTest, IgnoresMissingCollectionWhenOptionsAllowIt) {
    createDatabase(_opCtx.get(), kNss.db_forTest());
    // kInitialSync is the mode whose Options set allowNamespaceNotFoundErrorsOnCrudOps.
    const OplogApplier::Options options(OplogApplication::Mode::kInitialSync);

    ASSERT_OK(applyWorkItem(_opCtx.get(), options, makeItem({insertOp(kNss, BSON("_id" << 0))})));
    ASSERT_FALSE(collectionExists(_opCtx.get(), kNss));
}

TEST_F(PipelinedWorkItemTest, AppliesCommands) {
    const auto options = secondaryOptions();
    auto createOp = makeCreateCollectionOplogEntry(_opCtx.get(), nextOpTime(), kNss);

    ASSERT_OK(applyWorkItem(
        _opCtx.get(), options, makeItem({createOp, insertOp(kNss, BSON("_id" << 0))})));

    ASSERT_TRUE(collectionExists(_opCtx.get(), kNss));
    ASSERT_TRUE(docExists(_opCtx.get(), kNss, BSON("_id" << 0)));
}

TEST_F(PipelinedWorkItemTest, FallsBackToSingleInsertsWhenAnInjectedGroupedInsertFails) {
    createCollectionWithUuid(_opCtx.get(), kNss);
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
    createCollectionWithUuid(_opCtx.get(), kNss);
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
    createCollectionWithUuid(_opCtx.get(), kNss);

    // Each item depends on the one before it, so the final state is only correct in FIFO order.
    std::vector<WorkItem> items;
    items.push_back(makeItem({insertOp(kNss, BSON("_id" << 0))}));
    items.push_back(makeItem({updateOp(kNss, BSON("_id" << 0), BSON("a" << 1))}));
    items.push_back(makeItem({insertOp(kNss, BSON("_id" << 1))}));
    items.push_back(makeItem({deleteOp(kNss, BSON("_id" << 1))}));
    consumeOnWorker(secondaryOptions(), std::move(items));

    ASSERT_EQ(countDocs(kNss), 1);
    ASSERT_TRUE(docExists(_opCtx.get(), kNss, BSON("_id" << 0 << "a" << 1)));
}

using PipelinedWorkItemDeathTest = PipelinedWorkItemTest;

DEATH_TEST_REGEX_F(PipelinedWorkItemDeathTest, ConsumeRejectsEmptyWorkItem, "item.ops.empty") {
    std::vector<WorkItem> items(1);
    consumeOnWorker(secondaryOptions(), std::move(items));
}

DEATH_TEST_REGEX_F(PipelinedWorkItemDeathTest,
                   ConsumeFassertsOnUnrecoverableError,
                   "13322100.*Pipelined oplog applier worker failed to apply a work item") {
    createCollectionWithUuid(_opCtx.get(), kNss);
    const auto options = secondaryOptions();

    _opObserver->onInsertsFn =
        [&](OperationContext*, const NamespaceString&, const std::vector<BSONObj>&) {
            uasserted(ErrorCodes::OperationFailed, "injected unrecoverable failure");
        };

    std::vector<WorkItem> items;
    items.push_back(makeItem({insertOp(kNss, BSON("_id" << 0))}));
    consumeOnWorker(options, std::move(items));
}

// Runs the applier over a mock oplog buffer: the batcher pulls from the buffer, _run dispatches
// each batch, and the workers apply it to real collections. Each write is attributed to the worker
// thread that made it via the thread name the pool assigns,
// "PipelinedApplierWorker-<workerIdx>-<n>". A test may also gate on the write of a given document
// to hold that worker mid-batch or to learn when the document was written.
class PipelinedOplogApplierRunTest : public OplogApplierImplTest {
protected:
    static constexpr size_t kNumWorkers = 4;

    enum class WriteKind { kInsert, kDelete };

    // Tracks metadata about writes in tests, used to make correctness assertions.
    struct ObservedWrite {
        size_t workerIdx;
        WriteKind kind;
        NamespaceString nss;
        BSONObj doc;
        // Non-zero for inserts only: a unique ID shared by every document inserted in the same
        // onInserts call, so a test can determine whether or not an insert was batched.
        uint64_t insertCallId = 0;
    };

    // Set when the watched document is written by a worker; if 'hold' is set, that worker then
    // blocks until 'release' is set. Used to pause workers mid-batch to make correctness assertions
    // about concurrency.
    struct Gate {
        bool hold = false;
        Notification<void> reached;
        Notification<void> release;
    };

    void setUp() override {
        OplogApplierImplTest::setUp();
        ThreadPool::Options options;
        options.poolName = "PipelinedOplogApplierRunTest";
        options.maxThreads = 1;
        options.onCreateThread = [](const std::string& threadName) {
            Client::initThread(threadName, getGlobalServiceContext()->getService());
        };
        _executor = executor::ThreadPoolTaskExecutor::create(
            std::make_unique<ThreadPool>(options),
            std::make_unique<executor::NetworkInterfaceMock>());
        _executor->startup();
        _buffer.startup(_opCtx.get());

        _opObserver->onInsertsFn = [&](OperationContext*,
                                       const NamespaceString& nss,
                                       const std::vector<BSONObj>& docs) {
            auto workerIdx = currentWorkerIdx();
            auto callId = _nextInsertCallId++;
            {
                std::lock_guard lk(_mutex);
                for (const auto& doc : docs) {
                    _writes.push_back({workerIdx, WriteKind::kInsert, nss, doc.getOwned(), callId});
                }
            }
            for (const auto& doc : docs) {
                passGate(doc);
            }
        };
        _opObserver->onDeleteFn = [&](OperationContext*,
                                      const CollectionPtr& coll,
                                      StmtId,
                                      const BSONObj& doc,
                                      const OplogDeleteEntryArgs&) {
            {
                std::lock_guard lk(_mutex);
                _writes.push_back(
                    {currentWorkerIdx(), WriteKind::kDelete, coll->ns(), doc.getOwned()});
            }
            passGate(doc);
        };
    }

    void tearDown() override {
        // Never leave a worker held or the applier running past the test.
        for (auto& [id, gate] : _gates) {
            if (!gate.release) {
                gate.release.set();
            }
        }
        if (_finished) {
            shutdownApplier();
            _finished->get();
        }
        _applier.reset();
        _buffer.shutdown(_opCtx.get());
        _executor->shutdown();
        _executor->join();
        OplogApplierImplTest::tearDown();
    }

    static size_t currentWorkerIdx() {
        constexpr std::string_view kPrefix = "PipelinedApplierWorker-";
        auto name = getThreadName();
        invariant(name.starts_with(kPrefix), name);
        auto rest = name.substr(kPrefix.size());
        return static_cast<size_t>(std::stoul(std::string(rest.substr(0, rest.find('-')))));
    }

    // Registers a gate on the document with the given integer _id. Must be called before the
    // applier is started.
    Gate& watchWriteOf(int id, bool hold) {
        auto [it, inserted] = _gates.try_emplace(id);
        invariant(inserted);
        it->second.hold = hold;
        return it->second;
    }

    // Called on the worker thread for every write; marks and, if held, blocks on the doc's gate.
    void passGate(const BSONObj& doc) {
        auto idElem = doc["_id"];
        if (!idElem.isNumber()) {
            return;
        }
        auto it = _gates.find(idElem.numberInt());
        if (it == _gates.end()) {
            return;
        }
        auto& gate = it->second;
        if (!gate.reached) {
            gate.reached.set();
        }
        if (gate.hold) {
            gate.release.get();
        }
    }

    void waitForGate(Gate& gate) {
        ASSERT_TRUE(gate.reached.waitFor(_opCtx.get(), Seconds(60)));
    }

    void startApplier() {
        _applier = std::make_unique<PipelinedOplogApplier>(
            _executor.get(),
            &_buffer,
            &noopOplogApplierObserver,
            ReplicationCoordinator::get(serviceContext),
            getStorageInterface(),
            OplogApplier::Options(OplogApplication::Mode::kSecondary),
            kNumWorkers);
        _finished.emplace(_applier->startup());
    }

    // Requests shutdown. The batcher otherwise sleeps a full second on the drained buffer before
    // it notices the request, so that wait is skipped from here on.
    void shutdownApplier() {
        _skipBatcherWait.emplace("skipOplogBatcherWaitForData");
        _applier->shutdown();
    }

    // Shuts the applier down and waits for it to exit, which happens once it has dispatched
    // everything in the buffer and the workers have applied it.
    void finishApplier() {
        shutdownApplier();
        _finished->get();
        _finished.reset();
        ASSERT_TRUE(_buffer.isEmpty());
    }

    // Blocks until the applier has begun shutting its workers down, which it does only after the
    // batcher has stopped and every batch has been dispatched.
    void waitForWorkerShutdownToBegin(const unittest::LogCaptureGuard& logs) {
        const auto deadline = Date_t::now() + Seconds(60);
        while (logs.countTextContaining("Shutting down pipelined oplog applier workers") == 0) {
            ASSERT_LT(Date_t::now(), deadline);
            sleepmillis(1);
        }
    }

    void pushToBuffer(const std::vector<OplogEntry>& ops) {
        std::vector<BSONObj> docs;
        for (const auto& op : ops) {
            docs.push_back(op.getEntry().toBSON());
        }
        _buffer.push(_opCtx.get(), docs.begin(), docs.end());
    }

    // Runs a fresh applier over 'ops' until the workers have applied all of them.
    void applyThroughBuffer(const std::vector<OplogEntry>& ops) {
        pushToBuffer(ops);
        startApplier();
        finishApplier();
    }

    // The worker the router selects for 'op'.
    size_t expectedWorker(OplogEntry op) {
        PipelinedOpRouter router(kNumWorkers);
        return router.selectWorker(_opCtx.get(), &op);
    }

    // Given ops are hash-routed, a test cannot simply
    // pick _ids and know which worker are assigned. This searches upward from 'startFrom' for the
    // first _id whose document in kNss routes to 'workerIdx', giving tests a deterministic way to
    // build ops for a chosen worker: to spread a batch across every worker, to hold one worker
    // while another stays free, or to keep a payload on held workers.
    int idRoutedTo(size_t workerIdx, int startFrom = 0) {
        for (int id = startFrom;; ++id) {
            if (expectedWorker(makeInsertDocumentOplogEntry(
                    OpTime(Timestamp(1, 1), 1), kNss, BSON("_id" << id))) == workerIdx) {
                return id;
            }
        }
    }

    // The observed writes to 'nss', in observation order.
    std::vector<ObservedWrite> writesTo(const NamespaceString& nss) {
        std::lock_guard lk(_mutex);
        std::vector<ObservedWrite> writes;
        for (const auto& write : _writes) {
            if (write.nss == nss) {
                writes.push_back(write);
            }
        }
        return writes;
    }

    // Reconstructs the grouped insert calls to 'nss' from the flat _writes list. Every document
    // inserted in one onInserts call shares an insertCallId; this buckets those together, returning
    // one (workerIdx, docs) pair per call. A test uses the number of calls to verify that each
    // participating worker received exactly one work item per batch.
    std::vector<std::pair<size_t, std::vector<BSONObj>>> insertCallsTo(const NamespaceString& nss) {
        std::lock_guard lk(_mutex);
        std::map<uint64_t, std::pair<size_t, std::vector<BSONObj>>> byCallId;
        for (const auto& write : _writes) {
            if (write.kind != WriteKind::kInsert || write.nss != nss) {
                continue;
            }
            auto& [workerIdx, docs] = byCallId[write.insertCallId];
            workerIdx = write.workerIdx;
            docs.push_back(write.doc);
        }
        std::vector<std::pair<size_t, std::vector<BSONObj>>> calls;
        for (auto& [_, call] : byCallId) {
            calls.push_back(std::move(call));
        }
        return calls;
    }

    OplogEntry insertOp(const NamespaceString& nss, const BSONObj& doc) {
        return makeInsertDocumentOplogEntry(nextOpTime(), nss, doc);
    }

    static BSONObj setUpdate(const BSONObj& setDoc) {
        return update_oplog_entry::makeDeltaOplogEntry(
            BSON(doc_diff::kUpdateSectionFieldName << setDoc));
    }

    OplogEntry updateOp(const NamespaceString& nss, const BSONObj& query, const BSONObj& setDoc) {
        return makeUpdateDocumentOplogEntry(nextOpTime(), nss, query, setUpdate(setDoc));
    }

    OplogEntry deleteOp(const NamespaceString& nss, const BSONObj& query) {
        return makeDeleteDocumentOplogEntry(nextOpTime(), nss, query);
    }

    // A non-transactional applyOps entry holding 'innerOps'.
    OplogEntry applyOpsOp(std::vector<ReplOperation> innerOps) {
        return makeApplyOpsOplogEntry(nextOpTime(),
                                      std::move(innerOps),
                                      OperationSessionInfo(),
                                      Date_t::now(),
                                      {} /* stmtIds */,
                                      boost::none /* prevWriteOpTimeInTransaction */);
    }

    // A non-transactional applyOps entry holding inserts of 'docs' into 'nss'.
    OplogEntry applyOpsInsertingOp(const NamespaceString& nss,
                                   const UUID& uuid,
                                   const std::vector<BSONObj>& docs) {
        std::vector<ReplOperation> innerOps;
        for (const auto& doc : docs) {
            innerOps.push_back(MutableOplogEntry::makeInsertOperation(nss, uuid, doc, doc));
        }
        return applyOpsOp(std::move(innerOps));
    }

    // Creates a container keyed by bytes and returns its ident.
    std::string createBytesContainer() {
        auto* storageEngine = serviceContext->getStorageEngine();
        auto ident = storageEngine->generateNewInternalIdent();
        auto ru = storageEngine->newRecoveryUnit();
        StorageWriteTransaction swt(*ru);
        _container =
            storageEngine->getEngine()->makeInternalRecordStore(*ru, ident, KeyFormat::String);
        swt.commit();
        return ident;
    }

    OplogEntry containerOp(std::string_view ident, OpTypeEnum opType, const BSONObj& o) {
        return OplogEntry(DurableOplogEntry(DurableOplogEntryParams{
            .opTime = nextOpTime(),
            .opType = opType,
            .nss = NamespaceString::kContainerNamespace,
            .container = ident,
            .oField = o,
            .wallClockTime = Date_t::now(),
        }));
    }

    // A container op suitable for nesting in an applyOps entry.
    static ReplOperation containerReplOp(std::string_view ident,
                                         OpTypeEnum opType,
                                         const BSONObj& o) {
        ReplOperation op;
        op.setOpType(opType);
        op.setNss(NamespaceString::kContainerNamespace);
        op.setContainer(ident);
        op.setObject(o);
        return op;
    }

    static BSONBinData bytesKey(std::string_view key) {
        return BSONBinData(key.data(), key.size(), BinDataGeneral);
    }

    BSONObj packedInsertOf(const std::vector<std::string_view>& keys, BSONBinData value) {
        BSONArrayBuilder keyArray;
        for (auto key : keys) {
            keyArray.append(bytesKey(key));
        }
        return BSON("k" << keyArray.arr() << "v" << value);
    }

    // The container-op counterpart of idRoutedTo: the first keystring with n at or above
    // 'startFrom' whose single-key insert into 'ident' routes to 'workerIdx'.
    std::string containerKeyRoutedTo(std::string_view ident, size_t workerIdx, int startFrom = 0) {
        auto value = BSONBinData("V", 1, BinDataGeneral);
        for (int n = startFrom;; ++n) {
            auto key = "K" + std::to_string(n);
            if (expectedWorker(containerOp(ident,
                                           OpTypeEnum::kContainerInsert,
                                           BSON("k" << bytesKey(key) << "v" << value))) ==
                workerIdx) {
                return key;
            }
        }
    }

    bool containerHasKey(std::string_view ident, std::string_view key) {
        auto* kvEngine = serviceContext->getStorageEngine()->getEngine();
        auto& ru = *shard_role_details::getRecoveryUnit(_opCtx.get());
        return kvEngine->getDirectCursor(ru, ident)
            ->get(std::span<const char>(key.data(), key.size()))
            .isOK();
    }

    long long numRecords(const NamespaceString& nss) {
        auto coll = acquireCollection(
            _opCtx.get(),
            CollectionAcquisitionRequest(nss,
                                         PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                         ReadConcernArgs::get(_opCtx.get()),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);
        return coll.getCollectionPtr()->numRecords(_opCtx.get());
    }

    std::vector<BSONObj> readAllDocs(const NamespaceString& nss) {
        std::vector<BSONObj> docs;
        CollectionReader reader(_opCtx.get(), nss);
        while (true) {
            auto swDoc = reader.next();
            if (swDoc.getStatus() == ErrorCodes::CollectionIsEmpty) {
                return docs;
            }
            docs.push_back(unittest::assertGet(swDoc).getOwned());
        }
    }

    // Checks that a scan of 'nss' returns exactly 'expected' (compared by _id) and that the
    // collection's record count agrees with what the scan returned.
    void assertCollectionContainsExactly(const NamespaceString& nss,
                                         std::vector<BSONObj> expected) {
        auto actual = readAllDocs(nss);
        auto byId = [](const BSONObj& a, const BSONObj& b) {
            return a["_id"].woCompare(b["_id"]) < 0;
        };
        std::sort(actual.begin(), actual.end(), byId);
        std::sort(expected.begin(), expected.end(), byId);
        ASSERT_EQ(actual.size(), expected.size());
        for (size_t i = 0; i < actual.size(); ++i) {
            ASSERT_BSONOBJ_EQ(actual[i], expected[i]);
        }
        ASSERT_EQ(numRecords(nss), static_cast<long long>(actual.size()));
    }

    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;
    OplogBufferMock _buffer;
    std::unique_ptr<PipelinedOplogApplier> _applier;
    boost::optional<Future<void>> _finished;
    boost::optional<FailPointEnableBlock> _skipBatcherWait;
    std::mutex _mutex;
    std::atomic<uint64_t> _nextInsertCallId{1};
    std::vector<ObservedWrite> _writes;
    std::map<int, Gate> _gates;
    std::unique_ptr<RecordStore> _container;

    const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.poa_run");
    const NamespaceString kOtherNss =
        NamespaceString::createNamespaceString_forTest("test.poa_run_other");
};

TEST_F(PipelinedOplogApplierRunTest, AppliesEveryBatchInTheBufferOnTheWorkers) {
    createCollectionWithUuid(_opCtx.get(), kNss);
    // Small batches so the loop pulls and dispatches several of them.
    setServerParameter("replBatchLimitOperations", 8);

    std::vector<OplogEntry> ops;
    std::vector<BSONObj> expected;
    for (int i = 0; i < 50; ++i) {
        ops.push_back(insertOp(kNss, BSON("_id" << i)));
        expected.push_back(BSON("_id" << i));
    }
    ops.push_back(updateOp(kNss, BSON("_id" << 1), BSON("a" << 1)));
    expected[1] = BSON("_id" << 1 << "a" << 1);
    ops.push_back(deleteOp(kNss, BSON("_id" << 2)));
    expected.erase(expected.begin() + 2);

    applyThroughBuffer(ops);

    assertCollectionContainsExactly(kNss, expected);
}

// Every op is applied by the worker the router selects for it, and the batch reaches more than one
// worker.
TEST_F(PipelinedOplogApplierRunTest, AppliesEachOpOnTheWorkerTheRouterSelects) {
    createCollectionWithUuid(_opCtx.get(), kNss);
    createCollectionWithUuid(_opCtx.get(), kOtherNss);
    // One document per worker in kNss, plus a few in another collection.
    std::vector<OplogEntry> ops;
    int nextId = 0;
    for (size_t workerIdx = 0; workerIdx < kNumWorkers; ++workerIdx) {
        nextId = idRoutedTo(workerIdx, nextId);
        ops.push_back(insertOp(kNss, BSON("_id" << nextId++)));
    }
    for (int i = 0; i < 4; ++i) {
        ops.push_back(insertOp(kOtherNss, BSON("_id" << i)));
    }
    // {nss, _id} -> workerIdx
    std::map<std::pair<NamespaceString, int>, size_t> expectedWorkerByDoc;
    for (const auto& op : ops) {
        expectedWorkerByDoc[{op.getNss(), op.getObject()["_id"].Int()}] = expectedWorker(op);
    }

    applyThroughBuffer(ops);

    std::lock_guard lk(_mutex);
    std::set<std::pair<NamespaceString, int>> observedDocs;
    std::set<size_t> observedWorkers;
    // Check that each write was an insert, that it was only written once, and it was applied by the
    // correct worker thread.
    for (const auto& write : _writes) {
        ASSERT_EQ(write.kind, WriteKind::kInsert);
        auto key = std::make_pair(write.nss, write.doc["_id"].Int());
        ASSERT_TRUE(observedDocs.insert(key).second) << "written twice: " << write.doc;
        ASSERT_EQ(write.workerIdx, expectedWorkerByDoc.at(key)) << write.doc;
        observedWorkers.insert(write.workerIdx);
    }
    ASSERT_EQ(observedDocs.size(), expectedWorkerByDoc.size());
    ASSERT_EQ(observedWorkers.size(), kNumWorkers);
}

// Ops on one document all reach the same worker and are applied in oplog order.
TEST_F(PipelinedOplogApplierRunTest, AppliesOpsOnOneDocumentOnOneWorkerInOplogOrder) {
    createCollectionWithUuid(_opCtx.get(), kNss);

    applyThroughBuffer({insertOp(kNss, BSON("_id" << 0)),
                        deleteOp(kNss, BSON("_id" << 0)),
                        insertOp(kNss, BSON("_id" << 0 << "a" << 1)),
                        deleteOp(kNss, BSON("_id" << 0)),
                        insertOp(kNss, BSON("_id" << 0 << "a" << 2))});

    auto writes = writesTo(kNss);
    ASSERT_EQ(writes.size(), 5);
    const std::vector<WriteKind> expectedKinds{WriteKind::kInsert,
                                               WriteKind::kDelete,
                                               WriteKind::kInsert,
                                               WriteKind::kDelete,
                                               WriteKind::kInsert};
    for (size_t i = 0; i < writes.size(); ++i) {
        ASSERT_EQ(writes[i].workerIdx, writes.front().workerIdx);
        ASSERT_EQ(writes[i].kind, expectedKinds[i]) << "write " << i;
    }
    assertCollectionContainsExactly(kNss, {BSON("_id" << 0 << "a" << 2)});
}

// A worker receives its whole slice of a batch as one work item: consecutive inserts on one
// collection in a single item are applied as one grouped insert, so each participating worker
// makes exactly one insert call holding all of its documents, in oplog order.
TEST_F(PipelinedOplogApplierRunTest, EnqueuesOneWorkItemPerParticipatingWorkerPerBatch) {
    createCollectionWithUuid(_opCtx.get(), kNss);
    std::vector<OplogEntry> ops;
    std::map<size_t, std::vector<BSONObj>> expectedDocsByWorker;
    for (int i = 0; i < 12; ++i) {
        ops.push_back(insertOp(kNss, BSON("_id" << i)));
        expectedDocsByWorker[expectedWorker(ops.back())].push_back(BSON("_id" << i));
    }
    // Ensure at least two workers were assigned ops.
    ASSERT_GT(expectedDocsByWorker.size(), 1);

    applyThroughBuffer(ops);

    auto calls = insertCallsTo(kNss);
    // Each worker should have one grouped insert call.
    ASSERT_EQ(calls.size(), expectedDocsByWorker.size());
    std::set<size_t> workersSeen;
    for (const auto& [workerIdx, docs] : calls) {
        ASSERT_TRUE(workersSeen.insert(workerIdx).second)
            << "worker " << workerIdx << " made more than one insert call";
        const auto& expectedDocs = expectedDocsByWorker.at(workerIdx);
        ASSERT_EQ(docs.size(), expectedDocs.size()) << "worker " << workerIdx;
        for (size_t i = 0; i < docs.size(); ++i) {
            ASSERT_BSONOBJ_EQ(docs[i], expectedDocs[i]);
        }
    }
}

// A non-transactional applyOps entry is never applied as a unit: each inner op is routed as if it
// had been its own oplog entry.
TEST_F(PipelinedOplogApplierRunTest, RoutesTheInnerOpsOfANonTransactionalApplyOps) {
    auto uuid = createCollectionWithUuid(_opCtx.get(), kNss);
    auto applyOps =
        applyOpsInsertingOp(kNss, uuid, {BSON("_id" << 0), BSON("_id" << 1), BSON("_id" << 2)});
    auto plainInsert = insertOp(kNss, BSON("_id" << 3));
    std::map<int, size_t> expectedWorkerById;
    for (auto& innerOp : ApplyOps::extractOperations(applyOps)) {
        expectedWorkerById[innerOp.getObject()["_id"].Int()] = expectedWorker(innerOp);
    }
    expectedWorkerById[3] = expectedWorker(plainInsert);

    applyThroughBuffer({applyOps, plainInsert});

    auto writes = writesTo(kNss);
    std::set<int> observedIds;
    for (const auto& write : writes) {
        ASSERT_EQ(write.kind, WriteKind::kInsert);
        auto id = write.doc["_id"].Int();
        ASSERT_TRUE(observedIds.insert(id).second) << "written twice: " << write.doc;
        ASSERT_EQ(write.workerIdx, expectedWorkerById.at(id)) << write.doc;
    }
    ASSERT_EQ(observedIds, (std::set<int>{0, 1, 2, 3}));
    assertCollectionContainsExactly(
        kNss, {BSON("_id" << 0), BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 3)});
}

// Ops on one document keep their oplog order whether they arrive as plain entries, inside one
// applyOps entry, across applyOps entries, or across batch boundaries.
TEST_F(PipelinedOplogApplierRunTest, AppliesDependentOpsInsideAndOutsideApplyOpsInOplogOrder) {
    auto uuid = createCollectionWithUuid(_opCtx.get(), kNss);
    // Every outer oplog entry in its own batch.
    setServerParameter("replBatchLimitOperations", 1);

    applyThroughBuffer({
        insertOp(kNss, BSON("_id" << 0 << "a" << 0)),
        // Two dependent updates of the same document inside one applyOps: only the second may win.
        applyOpsOp({MutableOplogEntry::makeUpdateOperation(
                        kNss, uuid, setUpdate(BSON("a" << 1)), BSON("_id" << 0)),
                    MutableOplogEntry::makeUpdateOperation(
                        kNss, uuid, setUpdate(BSON("a" << 2)), BSON("_id" << 0)),
                    MutableOplogEntry::makeInsertOperation(
                        kNss, uuid, BSON("_id" << 1 << "b" << 0), BSON("_id" << 1))}),
        deleteOp(kNss, BSON("_id" << 1)),
        applyOpsOp({MutableOplogEntry::makeInsertOperation(
            kNss, uuid, BSON("_id" << 1 << "b" << 1), BSON("_id" << 1))}),
        updateOp(kNss, BSON("_id" << 1), BSON("c" << 1)),
    });

    assertCollectionContainsExactly(
        kNss, {BSON("_id" << 0 << "a" << 2), BSON("_id" << 1 << "b" << 1 << "c" << 1)});
}

// A packed container op is split into single-key ops before routing, so a later op on one of its
// keys is applied after it by the same worker.
TEST_F(PipelinedOplogApplierRunTest, ExpandsPackedContainerOpsBeforeRouting) {
    auto ident = createBytesContainer();
    auto value = BSONBinData("V", 1, BinDataGeneral);

    applyThroughBuffer(
        {containerOp(
             ident, OpTypeEnum::kContainerInsert, packedInsertOf({"K1", "K2", "K3"}, value)),
         containerOp(ident, OpTypeEnum::kContainerDelete, BSON("k" << bytesKey("K2")))});

    ASSERT_TRUE(containerHasKey(ident, "K1"));
    ASSERT_FALSE(containerHasKey(ident, "K2"));
    ASSERT_TRUE(containerHasKey(ident, "K3"));
}

// A packed container op nested in an applyOps entry is expanded too, so a later delete of one of
// its keys is applied after the insert of that key.
TEST_F(PipelinedOplogApplierRunTest, ExpandsPackedContainerOpsInsideApplyOps) {
    auto uuid = createCollectionWithUuid(_opCtx.get(), kNss);
    auto ident = createBytesContainer();
    auto value = BSONBinData("V", 1, BinDataGeneral);

    applyThroughBuffer(
        {applyOpsOp({MutableOplogEntry::makeInsertOperation(
                         kNss, uuid, BSON("_id" << 0), BSON("_id" << 0)),
                     containerReplOp(ident,
                                     OpTypeEnum::kContainerInsert,
                                     packedInsertOf({"K1", "K2", "K3"}, value))}),
         containerOp(ident, OpTypeEnum::kContainerDelete, BSON("k" << bytesKey("K2")))});

    ASSERT_TRUE(containerHasKey(ident, "K1"));
    ASSERT_FALSE(containerHasKey(ident, "K2"));
    ASSERT_TRUE(containerHasKey(ident, "K3"));
    assertCollectionContainsExactly(kNss, {BSON("_id" << 0)});
}

// A packed container op with no keys writes nothing, so it must not produce a work item.
TEST_F(PipelinedOplogApplierRunTest, IgnoresAPackedContainerOpWithNoKeys) {
    auto ident = createBytesContainer();
    auto value = BSONBinData("V", 1, BinDataGeneral);
    applyThroughBuffer(
        {containerOp(ident, OpTypeEnum::kContainerInsert, packedInsertOf({}, value))});
}

// The collection's in-memory record count follows the writes the workers make.
TEST_F(PipelinedOplogApplierRunTest, FastCountFollowsTheWorkersWrites) {
    auto uuid = createCollectionWithUuid(_opCtx.get(), kNss);
    std::vector<OplogEntry> ops;
    std::vector<BSONObj> expected;
    for (int i = 0; i < 10; ++i) {
        ops.push_back(insertOp(kNss, BSON("_id" << i)));
        expected.push_back(BSON("_id" << i));
    }
    ops.push_back(updateOp(kNss, BSON("_id" << 3), BSON("a" << 1)));
    expected[3] = BSON("_id" << 3 << "a" << 1);
    ops.push_back(deleteOp(kNss, BSON("_id" << 7)));
    expected.erase(expected.begin() + 7);
    ops.push_back(applyOpsInsertingOp(kNss, uuid, {BSON("_id" << 10), BSON("_id" << 11)}));
    expected.push_back(BSON("_id" << 10));
    expected.push_back(BSON("_id" << 11));

    applyThroughBuffer(ops);

    // assertCollectionContainsExactly compares the record count against a scan of the documents.
    assertCollectionContainsExactly(kNss, expected);
}

// The dispatcher does not wait for a batch to finish: a batch pushed while a worker is still busy
// with the previous batch is dispatched and applied by the other workers.
TEST_F(PipelinedOplogApplierRunTest, DispatchesTheNextBatchWhileAnEarlierBatchIsStillApplying) {
    createCollectionWithUuid(_opCtx.get(), kNss);
    const int heldId = idRoutedTo(0);
    const int laterId = idRoutedTo(1);
    auto& held = watchWriteOf(heldId, true /* hold */);
    auto& later = watchWriteOf(laterId, false /* hold */);

    pushToBuffer({insertOp(kNss, BSON("_id" << heldId))});
    startApplier();
    waitForGate(held);

    // Pushed after the applier started, while worker 0 is held mid-batch.
    pushToBuffer({insertOp(kNss, BSON("_id" << laterId))});
    waitForGate(later);

    // Shutdown is requested while the worker is still held. The applier gets as far as shutting
    // its workers down, then cannot exit until the held worker is released and finishes its item.
    unittest::LogCaptureGuard logs;
    shutdownApplier();
    waitForWorkerShutdownToBegin(logs);
    ASSERT_FALSE(_finished->isReady());
    held.release.set();
    _finished->get();
    _finished.reset();

    assertCollectionContainsExactly(kNss, {BSON("_id" << heldId), BSON("_id" << laterId)});
}

// Derived ops (from applyOps) live in a local vector inside _dispatchOps. This holds a worker
// mid-batch, lets the dispatcher move past it, and verifies the derived ops are still applied
// correctly — they must be owned by the work item.
TEST_F(PipelinedOplogApplierRunTest, WorkItemsOwnDerivedOpsAfterTheDispatcherMovesOn) {
    auto uuid = createCollectionWithUuid(_opCtx.get(), kNss);
    // One entry per batch so the marker is dispatched strictly after the applyOps.
    setServerParameter("replBatchLimitOperations", 1);

    const int heldId = idRoutedTo(0);
    auto& held = watchWriteOf(heldId, true /* hold */);
    const int derivedId = idRoutedTo(0, heldId + 1);
    const int markerId = idRoutedTo(1, derivedId + 1);
    auto& marker = watchWriteOf(markerId, false /* hold */);

    pushToBuffer({insertOp(kNss, BSON("_id" << heldId))});
    startApplier();
    // Worker 0 is blocked after completing this write.
    waitForGate(held);

    // The derived op from the apply ops is also routed to worker 0, where it sits waiting in the
    // queue. The marker insert oplog entry is routed to worker 1.
    pushToBuffer({applyOpsInsertingOp(kNss, uuid, {BSON("_id" << derivedId)}),
                  insertOp(kNss, BSON("_id" << markerId))});
    // The marker oplog entry is completed by worker 1, while the derived op from the apply ops is
    // still blocked.
    waitForGate(marker);

    // Worker 0 is able to apply the derived insert.
    held.release.set();
    finishApplier();

    assertCollectionContainsExactly(
        kNss, {BSON("_id" << heldId), BSON("_id" << derivedId), BSON("_id" << markerId)});
}


using PipelinedOplogApplierRunDeathTest = PipelinedOplogApplierRunTest;

DEATH_TEST_REGEX_F(PipelinedOplogApplierRunDeathTest,
                   FassertsOnABatchThatIsNotAfterLastApplied,
                   "13322700.*does not follow the last dispatched op") {
    createCollectionWithUuid(_opCtx.get(), kNss);
    auto op = insertOp(kNss, BSON("_id" << 0));
    ReplicationCoordinator::get(serviceContext)
        ->setMyLastAppliedOpTimeAndWallTimeForward({op.getOpTime(), Date_t::now()});
    applyThroughBuffer({op});
}

// The bound is the last dispatched op, not lastApplied, which lags behind dispatch.
DEATH_TEST_REGEX_F(PipelinedOplogApplierRunDeathTest,
                   FassertsOnABatchThatPrecedesTheLastDispatchedOp,
                   "13322700.*does not follow the last dispatched op") {
    createCollectionWithUuid(_opCtx.get(), kNss);
    setServerParameter("replBatchLimitOperations", 1);
    applyThroughBuffer(
        {makeInsertDocumentOplogEntry(OpTime(Timestamp(100, 0), 1), kNss, BSON("_id" << 0)),
         makeInsertDocumentOplogEntry(OpTime(Timestamp(50, 0), 1), kNss, BSON("_id" << 1))});
}

DEATH_TEST_REGEX_F(PipelinedOplogApplierRunDeathTest,
                   FassertsOnABatchThatRepeatsTheLastDispatchedOpTime,
                   "13322700.*does not follow the last dispatched op") {
    createCollectionWithUuid(_opCtx.get(), kNss);
    setServerParameter("replBatchLimitOperations", 1);
    applyThroughBuffer(
        {makeInsertDocumentOplogEntry(OpTime(Timestamp(100, 0), 1), kNss, BSON("_id" << 0)),
         makeInsertDocumentOplogEntry(OpTime(Timestamp(100, 0), 1), kNss, BSON("_id" << 1))});
}

DEATH_TEST_REGEX_F(PipelinedOplogApplierRunDeathTest,
                   FassertsWhenABatchCannotBeDispatched,
                   "13322701.*failed to dispatch a batch") {
    // A non-transactional applyOps whose inner op cannot be extracted.
    applyThroughBuffer(
        {makeCommandOplogEntry(nextOpTime(),
                               NamespaceString::makeCommandNamespace(DatabaseName::kAdmin),
                               BSON("applyOps" << BSON_ARRAY(BSONObj())))});
}

DEATH_TEST_REGEX_F(PipelinedOplogApplierRunDeathTest,
                   AnOpRequiringInlineApplicationIsUnimplemented,
                   "Hit a MONGO_UNIMPLEMENTED") {
    applyThroughBuffer({makeCreateCollectionOplogEntry(_opCtx.get(), nextOpTime(), kNss)});
}

}  // namespace
}  // namespace mongo::repl
