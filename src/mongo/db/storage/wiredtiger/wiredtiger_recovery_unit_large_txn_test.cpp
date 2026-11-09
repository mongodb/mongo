// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/checked_cast.h"
#include "mongo/db/client.h"
#include "mongo/db/rss/attached_storage/attached_persistence_provider.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_extensions.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/scopeguard.h"

#include <string>
#include <utility>

#include <wiredtiger.h>

namespace mongo {
namespace {

//
// Tests for transactions whose commit log record exceeds WT's 4GB single-record limit.
// They need debug table logging so that writes to an unlogged table land in
// the transaction's log record, a large enough cache for the transaction's dirty data, and
// raised eviction triggers so the too-large-for-cache rollback does not fire first.
//
// These tests write 4GB+ per run and have a multi-GB memory peak (the dirty data plus the
// assembled log record).
//


class WiredTigerRecoveryUnitLargeTxnTest : public unittest::Test {
public:
    using ClientAndCtx =
        std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>;

    WiredTigerRecoveryUnitLargeTxnTest() : _dbpath("wt_large_txn_test") {
        if (!hasGlobalServiceContext()) {
            setGlobalServiceContext(ServiceContext::make());
        }
        auto& provider =
            rss::ReplicatedStorageService::get(getGlobalServiceContext()).getPersistenceProvider();
        WiredTigerKVEngineBase::WiredTigerConfig wtConfig =
            getWiredTigerConfigFromStartupOptions(provider);
        // The dirty footprint is ~4.34GB, so a 5GB cache with the eviction triggers raised to
        // 95% holds it with margin while keeping the test's peak memory low.
        wtConfig.cacheSizeMB = 5120;
        // Disable journal compression: snappy's worst-case buffer for a 4.3GB record adds
        // several GB to the test's peak memory. Disabling compression also makes the record
        // size deterministic.
        wtConfig.logCompressor = "none";
        wtConfig.extraOpenOptions =
            "eviction_dirty_trigger=95,eviction_updates_trigger=95,"
            "debug_mode=(table_logging=true,checkpoint_retention=1)";

        // Use a replica set so that replicated collections are not journaled, mirroring a
        // production node where the commit log record carries only the oplog entries.
        _engine = std::make_unique<WiredTigerKVEngine>(
            std::string{kWiredTigerEngineName},
            _dbpath.path(),
            &_cs,
            std::move(wtConfig),
            WiredTigerExtensions::get(getGlobalServiceContext()),
            provider,
            /*repair=*/false,
            /*isReplSet=*/true,
            /*shouldRecoverFromOplogAsStandalone=*/false,
            /*inStandaloneMode=*/false);

        _engine->notifyStorageStartupRecoveryComplete();
    }

    ~WiredTigerRecoveryUnitLargeTxnTest() {
        _engine->cleanShutdown(true);
    }

    ClientAndCtx makeClientAndOpCtx(const std::string& clientName) {
        auto client = getGlobalServiceContext()->getService()->makeClient(clientName);
        auto opCtx = client->makeOperationContext();
        shard_role_details::setRecoveryUnit(
            opCtx.get(),
            std::unique_ptr<RecoveryUnit>(_engine->newRecoveryUnit()),
            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
        return {std::move(client), std::move(opCtx)};
    }

    WiredTigerRecoveryUnit* getRu(OperationContext* opCtx) {
        return checked_cast<WiredTigerRecoveryUnit*>(shard_role_details::getRecoveryUnit(opCtx));
    }

    WiredTigerKVEngine* getEngine() {
        return _engine.get();
    }

    static WT_CURSOR* getCursor(WiredTigerRecoveryUnit* ru, const char* uri = kUri) {
        WT_CURSOR* cursor = nullptr;
        auto* session = ru->getSession();
        invariantWTOK(session->create(uri, kConfig), *session);
        invariantWTOK(session->open_cursor(uri, nullptr, nullptr, &cursor), *session);
        return cursor;
    }

    // The transaction's log record grows to ~kNumOps * kValueBytes. Journal compression is
    // disabled, so the content of the value does not matter.
    static const char* largeValue() {
        static const std::string value(kValueBytes, '\0');
        return value.c_str();
    }

    static void insertLargeOps(WiredTigerRecoveryUnit* ru, int numOps, const char* uri = kUri) {
        WT_CURSOR* cursor = getCursor(ru, uri);
        WT_ITEM value;
        value.data = const_cast<char*>(largeValue());
        value.size = kValueBytes;
        for (int i = 0; i < numOps; ++i) {
            cursor->set_key(cursor, std::to_string(i).c_str());
            cursor->set_value(cursor, &value);
            invariantWTOK(cursor->insert(cursor), *ru->getSession());
        }
        invariantWTOK(cursor->close(cursor), *ru->getSession());
    }

    static constexpr int kValueBytes = 14'000'000;
    // ~310 * 14MB = 4.34GB of logged bytes, over WT's 4,294,967,295-byte single record limit.
    static constexpr int kNumOps = 310;
    static constexpr const char* kSideUri = "table:large_txn_side";

private:
    static constexpr const char* kUri = "table:large_txn";
    static constexpr const char* kConfig = "key_format=S,value_format=u,log=(enabled=false)";

    unittest::TempDir _dbpath;
    ClockSourceMock _cs;
    std::unique_ptr<WiredTigerKVEngine> _engine;
};

TEST_F(WiredTigerRecoveryUnitLargeTxnTest, OversizedCommitLogRecordFailsWithTransactionTooLarge) {
    auto clientAndCtx = makeClientAndOpCtx("writer");
    auto* ru = getRu(clientAndCtx.second.get());
    ru->setOperationContext(clientAndCtx.second.get());

    ru->beginUnitOfWork(clientAndCtx.second->readOnly());
    ScopeGuard guard{[&] {
        ru->abortUnitOfWork();
    }};
    insertLargeOps(ru, kNumOps);

    // The commit log record exceeds the 4GB limit: the server must fail the commit with
    // TransactionTooLarge instead of crashing on the invariant.
    try {
        ru->commitUnitOfWork();
        FAIL("Expected commitUnitOfWork to throw TransactionTooLarge");
    } catch (const DBException& e) {
        ASSERT_EQ(e.code(), ErrorCodes::TransactionTooLarge);
    }

    // The recovery unit must have cleaned up after the failed commit and be reusable: the
    // abort below is the same one the WriteUnitOfWork destructor would run.
    guard.dismiss();
    ru->abortUnitOfWork();

    // The same recovery unit must accept a new unit of work after the failed commit.
    ru->beginUnitOfWork(clientAndCtx.second->readOnly());
    insertLargeOps(ru, 1);
    ru->commitUnitOfWork();

    auto clientAndCtx2 = makeClientAndOpCtx("writer2");
    auto* ru2 = getRu(clientAndCtx2.second.get());
    ru2->setOperationContext(clientAndCtx2.second.get());
    ru2->beginUnitOfWork(clientAndCtx2.second->readOnly());
    insertLargeOps(ru2, 1);
    ru2->commitUnitOfWork();
}

// Mimics the sharded-cluster prepare flow: a transaction is prepared while holding only its
// (unlogged) data writes, and the prepare oplog entries are then written and committed by a
// separate side transaction (see TransactionParticipant::SideTransactionBlock).
// When those oplog writes exceed the 4GB log record limit, the side transaction's commit must
// fail with TransactionTooLarge, the in-doubt prepared transaction must still abort cleanly, and
// the engine must stay healthy. The prepared transaction's own commit is not the oversized one
// in production, because WT rejects logged tables inside prepared transactions (EINVAL in
// __wt_txn_prepare), so only unlogged modifications can be prepared.
TEST_F(WiredTigerRecoveryUnitLargeTxnTest,
       OversizedPrepareSideTransactionFailsAndPreparedTxnStillAborts) {
    auto clientAndCtx = makeClientAndOpCtx("writer");
    auto* ru = getRu(clientAndCtx.second.get());
    ru->setOperationContext(clientAndCtx.second.get());

    // The user's transaction: small data writes, then prepared and left in-doubt, as it is while
    // the side transaction writes the prepare oplog entries.
    ru->beginUnitOfWork(clientAndCtx.second->readOnly());
    insertLargeOps(ru, 1);
    getEngine()->setStableTimestamp({1, 1}, false);
    ru->setPrepareTimestamp({2, 1});
    ru->prepareUnitOfWork();

    // The side transaction carrying the prepare oplog entries exceeds the 4GB record limit.
    auto sideClientAndCtx = makeClientAndOpCtx("side");
    auto* sideRu = getRu(sideClientAndCtx.second.get());
    sideRu->setOperationContext(sideClientAndCtx.second.get());
    sideRu->beginUnitOfWork(sideClientAndCtx.second->readOnly());
    insertLargeOps(sideRu, kNumOps, kSideUri);
    try {
        sideRu->commitUnitOfWork();
        FAIL("Expected commitUnitOfWork to throw TransactionTooLarge");
    } catch (const DBException& e) {
        ASSERT_EQ(e.code(), ErrorCodes::TransactionTooLarge);
    }
    sideRu->abortUnitOfWork();

    // The in-doubt prepared transaction must still abort cleanly.
    ru->abortUnitOfWork();

    // The engine must stay healthy for new work.
    auto checkClientAndCtx = makeClientAndOpCtx("check");
    auto* checkRu = getRu(checkClientAndCtx.second.get());
    checkRu->setOperationContext(checkClientAndCtx.second.get());
    checkRu->beginUnitOfWork(checkClientAndCtx.second->readOnly());
    insertLargeOps(checkRu, 1);
    checkRu->commitUnitOfWork();
}

}  // namespace
}  // namespace mongo
