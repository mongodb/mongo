/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <cstddef>
#include <fmt/format.h>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/snapshot_window_options_gen.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"  // for WiredTigerSession
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_component_settings.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

/**
 * This suite holds test cases that run against the WiredTiger KV Engine without the support
 * of the KVEngine test harness.
 *
 * The goal of this suite is to support test cases where the defaults provided by the test
 * harness are not required or desired. This suite is also intended to support a mix of
 * operations on both the KVEngine and the lower-level WiredTiger C interface.
 */

/**
 * This ClientObserver is registered with the ServiceContext to ensure that
 * the OperationContext is constructed with a WiredTigerRecoveryUnit rather than
 * the default RecoveryUnitNoop.
 */
class KVTestClientObserver final : public ServiceContext::ClientObserver {
public:
    void onCreateClient(Client* client) override {}
    void onDestroyClient(Client* client) override {}
    void onCreateOperationContext(OperationContext* opCtx) override {
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        shard_role_details::setRecoveryUnit(
            opCtx,
            std::unique_ptr<RecoveryUnit>(_kvEngine->newRecoveryUnit()),
            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    }
    void onDestroyOperationContext(OperationContext* opCtx) override {}

    void setKVEngine(KVEngine* kvEngine) {
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        _kvEngine = kvEngine;
    }

private:
    stdx::mutex _mutex;  // protects _kvEngine
    KVEngine* _kvEngine;
};

/**
 * Returns a new instance of the WiredTigerKVEngine.
 */
std::unique_ptr<WiredTigerKVEngine> makeKVEngine(ServiceContext* serviceContext,
                                                 const std::string& path,
                                                 ClockSource* clockSource) {
    return std::make_unique<WiredTigerKVEngine>(
        /*canonicalName=*/"",
        path,
        clockSource,
        /*extraOpenOptions=*/"",
        // Refer to config string in WiredTigerCApiTest::RollbackToStable40.
        /*cacheSizeMB=*/1,
        /*maxHistoryFileSizeMB=*/0,
        /*ephemeral=*/false,
        /*repair=*/false);
}

/**
 * Returns std::string stored in RecordData.
 */
std::string toString(const RecordData& recordData) {
    return std::string{recordData.data(), static_cast<std::size_t>(recordData.size())};
}

/**
 * Commits WriteUnitOfWork and checks timestamp of committed storage transaction.
 */
void commitWriteUnitOfWork(OperationContext* opCtx,
                           WriteUnitOfWork& wuow,
                           Timestamp expectedCommitTimestamp) {
    bool isCommitted = false;
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [&](OperationContext*, boost::optional<Timestamp> commitTimestamp) {
            ASSERT(commitTimestamp) << "Storage transaction committed without timestamp";
            ASSERT_EQ(*commitTimestamp, expectedCommitTimestamp);
            isCommitted = true;
        });
    ASSERT_FALSE(isCommitted);
    wuow.commit();
    ASSERT(isCommitted);
}

}  // namespace
}  // namespace mongo
