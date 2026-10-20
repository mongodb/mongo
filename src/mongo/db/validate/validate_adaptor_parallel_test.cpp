// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/validate/validate_adaptor_parallel.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/validate/validate_options.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/db/validate/validate_state.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::collection_validation {
using namespace std::literals::string_view_literals;

/**
 * A slice cursor that never reaches the end of its record store: seek() and every next() return a
 * freshly generated document, with record ids that keep increasing from the one it was seeked to.
 * A slice reading through it is always in the middle of scanning, so the only way for its
 * traversal to end is a yield that throws. The test collections place their records so far apart
 * that the traversal's end bound for the slice is never reached.
 */
class EndlessSliceValidateCursor final : public SliceValidateCursor {
public:
    using SliceValidateCursor::SliceValidateCursor;

    boost::optional<Record> seek(const RecordId& recordId) override {
        _nextId = recordId.getLong();
        return generate();
    }
    boost::optional<Record> next() override {
        return generate();
    }

private:
    boost::optional<Record> generate() {
        const auto id = _nextId++;
        _doc = BSON("_id" << id);
        return Record{RecordId(id), RecordData(_doc.objdata(), _doc.objsize())};
    }

    int64_t _nextId{0};
    BSONObj _doc;
};

class InfiniteTestAdaptor final : public ValidateAdaptorParallel {
public:
    // 'bar', if given, is waited on once the validation owns the shared thread pool.
    InfiniteTestAdaptor(OperationContext* opCtx,
                        ValidateState& state,
                        int64_t targetRecordsPerSlice,
                        unittest::Barrier* bar = nullptr)
        : ValidateAdaptorParallel(opCtx, state, targetRecordsPerSlice), _bar(bar) {}

protected:
    void onThreadPoolAcquired_forTest() const override {
        if (_bar) {
            _bar->countDownAndWait();
        }
    }

    std::unique_ptr<ValidateCursor> makeSliceCursor(OperationContext* workerOpCtx,
                                                    const RecordStore& recordStore,
                                                    CancellationToken cancellation) const override {
        return std::make_unique<EndlessSliceValidateCursor>(
            workerOpCtx,
            recordStore.getCursor(workerOpCtx, *shard_role_details::getRecoveryUnit(workerOpCtx)),
            std::move(cancellation));
    }

private:
    unittest::Barrier* _bar;
};

namespace {

const auto kNssA = NamespaceString::createNamespaceString_forTest("validateParallelDB.collA"sv);
const auto kNssB = NamespaceString::createNamespaceString_forTest("validateParallelDB.collB"sv);

// Spacing between the ids of the records seeded below. Slice boundaries are strided across the
// first to the last record id, so each slice then spans a range far too wide for the endless
// cursor to ever reach the slice's end.
constexpr int64_t kRecordIdStride{int64_t{1} << 40};

// How long a cancelled validation may take to return before it is considered hung.
constexpr Seconds kHangTimeout{10};

class ValidateAdaptorParallelTest : public CatalogTestFixture {
protected:
    // Creates 'nss' holding 'numRecords' records. They are inserted straight into the record
    // store, so they are not consistent with its indexes, which does not matter to a traversal
    // that never reaches its end.
    void createCollectionWithSparseRecords(const NamespaceString& nss, int numRecords) {
        auto opCtx = operationContext();
        ASSERT_OK(storageInterface()->createCollection(opCtx, nss, {}));
        const AutoGetCollection coll(opCtx, nss, MODE_IX);
        const auto doc = BSON("_id" << 0);
        WriteUnitOfWork wuow(opCtx);
        for (int i = 1; i <= numRecords; ++i) {
            ASSERT_OK(
                coll->getRecordStore()->insertRecord(opCtx,
                                                     *shard_role_details::getRecoveryUnit(opCtx),
                                                     RecordId(i * kRecordIdStride),
                                                     doc.objdata(),
                                                     doc.objsize(),
                                                     Timestamp()));
        }
        wuow.commit();
    }
};

// Validations whose slices never run out of records can only end by being cancelled. Cancel two at
// once, with more slices than the shared pool has workers, and make sure both return rather than
// leaving slices stuck in the pool.
TEST_F(ValidateAdaptorParallelTest, CancellingTwoValidationsDoesNotHang) {
    // More slices per validation than workers in the pool.
    const int numSlices = 2 * ProcessInfo::getNumAvailableCores();
    unittest::ServerParameterGuard maxSlices{"validateParallelMaxRecordStoreSlices",
                                             std::max(numSlices, 256)};
    const std::vector<NamespaceString> namespaces{kNssA, kNssB};
    for (const auto& nss : namespaces) {
        createCollectionWithSparseRecords(nss, numSlices);
    }

    struct Validation {
        ServiceContext::UniqueClient client;
        ServiceContext::UniqueOperationContext opCtx;
        Client* clientPtr;
        OperationContext* opCtxPtr;
        PromiseAndFuture<void> result;
    };
    std::vector<Validation> validations;
    for (size_t i = 0; i < namespaces.size(); ++i) {
        auto client = getServiceContext()->getService()->makeClient("validate");
        auto opCtx = client->makeOperationContext();
        Client* clientPtr = client.get();
        OperationContext* opCtxPtr = opCtx.get();
        validations.push_back(
            {std::move(client), std::move(opCtx), clientPtr, opCtxPtr, PromiseAndFuture<void>{}});
    }

    // Both validations start together, and so do both cancellations, which then land while the
    // validations are in flight together.
    unittest::Barrier startValidations(namespaces.size());
    unittest::Barrier startCancellations(namespaces.size());
    std::vector<unittest::JoinThread> threads;
    for (size_t i = 0; i < namespaces.size(); ++i) {
        auto& validation = validations[i];
        threads.emplace_back([&, nss = namespaces[i]] {
            AlternativeClientRegion acr(validation.client);
            startValidations.countDownAndWait();
            auto* opCtx = validation.opCtx.get();
            Status status = Status::OK();
            try {
                ValidateState state(opCtx,
                                    nss,
                                    ValidationOptions(ValidateMode::kForeground,
                                                      RepairMode::kNone,
                                                      /*logDiagnostics=*/false,
                                                      currentValidationVersion,
                                                      /*verifyConfigurationOverride=*/boost::none,
                                                      /*readTimestamp=*/boost::none,
                                                      /*hashPrefixes=*/boost::none,
                                                      /*revealHashedIds=*/boost::none,
                                                      /*targetRecordsPerRecordStoreSlice=*/1));
                uassertStatusOK(state.initializeCollection(opCtx));
                state.initializeCursors(opCtx);
                // As validate() does before traversing, since merging slice results requires it.
                ValidateResults results;
                results.setNamespaceString(state.nss());
                results.setUUID(state.uuid());
                results.setRepairMode(state.getRepairMode());
                results.setReadTimestamp(state.getReadTimestamp());
                InfiniteTestAdaptor(opCtx, state, /*targetRecordsPerSlice=*/1)
                    .traverseRecordStore(opCtx, results, currentValidationVersion);
            } catch (const DBException& e) {
                status = e.toStatus();
            }
            if (status.isOK()) {
                validation.result.promise.emplaceValue();
            } else {
                validation.result.promise.setError(status);
            }
        });
        threads.emplace_back([&] {
            startCancellations.countDownAndWait();
            // Let the slices get going, so the cancellation lands mid-scan.
            sleepmillis(200);
            // The validation thread owns the client for the duration, so use the raw pointers.
            ClientLock lk(validation.clientPtr);
            getServiceContext()->killOperation(lk, validation.opCtxPtr, ErrorCodes::Interrupted);
        });
    }

    for (auto& validation : validations) {
        // Failing the test is only possible if the validation returns, as the threads are joined
        // on the way out, so a hang is turned into a crash with a message instead.
        const auto status = operationContext()->runWithDeadline(
            Date_t::now() + kHangTimeout, ErrorCodes::ExceededTimeLimit, [&] {
                return std::move(validation.result.future).getNoThrow(operationContext());
            });
        if (status.code() == ErrorCodes::ExceededTimeLimit) {
            LOGV2_FATAL(11157102,
                        "Cancelled validation did not return within the timeout",
                        "timeout"_attr = kHangTimeout);
        }
        ASSERT_EQ(ErrorCodes::Interrupted, status.code());
    }
}

// Every validation in flight shares one pool, and the pool is torn down when the last of them lets
// go, so that a later validation builds a new one.
TEST_F(ValidateAdaptorParallelTest, ValidationsShareOnePoolUntilTheLastLetsGo) {
    // More than one slice, so that the validations run in parallel at all.
    const int numSlices = 2 * ProcessInfo::getNumAvailableCores();
    unittest::ServerParameterGuard maxSlices{"validateParallelMaxRecordStoreSlices",
                                             std::max(numSlices, 256)};

    const std::vector namespaces = {kNssA, kNssB};
    for (const auto& nss : namespaces) {
        createCollectionWithSparseRecords(nss, numSlices);
    }

    unittest::Barrier cursorsBarrier{namespaces.size() + 1};
    struct Validation {
        ServiceContext::UniqueClient client;
        ServiceContext::UniqueOperationContext opCtx;
        Client* clientPtr{nullptr};
        OperationContext* opCtxPtr{nullptr};
        Status status = Status::OK();
        std::unique_ptr<unittest::JoinThread>
            thread;  // Last, so it is joined before the rest dies.
    };
    // Heap allocated, as the threads refer to their Validation.
    std::vector<std::unique_ptr<Validation>> validations;
    for (const auto& nss : namespaces) {
        auto& validation = validations.emplace_back(std::make_unique<Validation>());
        validation->client = getServiceContext()->getService()->makeClient("validate");
        validation->opCtx = validation->client->makeOperationContext();
        validation->clientPtr = validation->client.get();
        validation->opCtxPtr = validation->opCtx.get();
        validation->thread = std::make_unique<unittest::JoinThread>(
            [&cursorsBarrier = cursorsBarrier, &validation = *validation, nss] {
                AlternativeClientRegion acr(validation.client);
                auto* opCtx = validation.opCtx.get();
                try {
                    ValidateState state(
                        opCtx,
                        nss,
                        ValidationOptions(ValidateMode::kForeground,
                                          RepairMode::kNone,
                                          /*logDiagnostics=*/false,
                                          currentValidationVersion,
                                          /*verifyConfigurationOverride=*/boost::none,
                                          /*readTimestamp=*/boost::none,
                                          /*hashPrefixes=*/boost::none,
                                          /*revealHashedIds=*/boost::none,
                                          /*targetRecordsPerRecordStoreSlice=*/1));
                    uassertStatusOK(state.initializeCollection(opCtx));
                    state.initializeCursors(opCtx);
                    ValidateResults results;
                    results.setNamespaceString(state.nss());
                    results.setUUID(state.uuid());
                    results.setRepairMode(state.getRepairMode());
                    results.setReadTimestamp(state.getReadTimestamp());
                    InfiniteTestAdaptor(opCtx, state, /*targetRecordsPerSlice=*/1, &cursorsBarrier)
                        .traverseRecordStore(opCtx, results, currentValidationVersion);
                } catch (const DBException& e) {
                    validation.status = e.toStatus();
                }
            });
    }

    // The validations never finish by themselves, so make sure they are cancelled however the test
    // exits, or else joining their threads would hang it instead of reporting a failure.
    const auto cancelAll = [&] {
        for (const auto& validation : validations) {
            ClientLock lk(validation->clientPtr);
            getServiceContext()->killOperation(lk, validation->opCtxPtr, ErrorCodes::Interrupted);
        }
    };
    ScopeGuard cancelOnExit(cancelAll);

    cursorsBarrier.countDownAndWait();

    std::weak_ptr<ThreadPool> firstPool;
    {
        const auto pool = getValidateThreadPool();
        ASSERT_TRUE(pool);
        // Asking again, with both still in progress, gets the very same pool.
        ASSERT_EQ(pool, getValidateThreadPool());
        firstPool = pool;
    }

    // Cancel both: with no owner left, the pool is torn down. Both are cancelled before either is
    // joined, as the shared pool is first-in-first-out and so a cancelled validation's queued
    // slices wait behind the other's, which never finish until it is cancelled too.
    cancelAll();
    cancelOnExit.dismiss();
    for (const auto& validation : validations) {
        validation->thread->join();
        ASSERT_EQ(ErrorCodes::Interrupted, validation->status.code());
    }
    ASSERT_TRUE(firstPool.expired());

    // A later validation therefore gets a pool of its own, not the old one.
    const auto newPool = getValidateThreadPool();
    ASSERT_TRUE(newPool);
    // It is possible for the new pool to have the same address, so only check that the weak_ptr has
    // expired.
    ASSERT_TRUE(firstPool.expired());
}

}  // namespace

}  // namespace mongo::collection_validation
