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

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>

#include <wiredtiger.h>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {
// All replicated collections are not logged.
static constexpr bool kIsLogged = false;

class WiredTigerIndexHarnessHelper final : public SortedDataInterfaceHarnessHelper {
public:
    WiredTigerIndexHarnessHelper(int32_t cacheSizeMB) : _dbpath("wt_test"), _conn(nullptr) {
        std::string config = fmt::format("create,cache_size={}M,", cacheSizeMB);
        int ret = wiredtiger_open(_dbpath.path().c_str(), nullptr, config.c_str(), &_conn);
        invariantWTOK(ret, nullptr);

        _fastClockSource = std::make_unique<SystemClockSource>();
        _connection = std::make_unique<WiredTigerConnection>(
            _conn, _fastClockSource.get(), /*sessionCacheMax=*/33000);
    }

    ~WiredTigerIndexHarnessHelper() final {
        _connection.reset();
        _conn->close(_conn, nullptr);
    }

    std::unique_ptr<SortedDataInterface> newIdIndexSortedDataInterface(
        OperationContext* opCtx) final {
        std::string ns = "test.wt";
        std::string indexName = "_id";
        NamespaceString nss = NamespaceString::createNamespaceString_forTest(ns);
        BSONObj spec =
            BSON("key" << BSON("_id" << 1) << "name" << indexName << "v"
                       << static_cast<int>(IndexConfig::kLatestIndexVersion) << "unique" << true);

        auto ordering = Ordering::allAscending();
        IndexConfig config{true /* isIdIndex */,
                           true /* unique */,
                           IndexConfig::kLatestIndexVersion,
                           spec,
                           indexName,
                           ordering};

        StatusWith<std::string> result =
            WiredTigerIndex::generateCreateString(std::string{kWiredTigerEngineName},
                                                  "",
                                                  "",
                                                  NamespaceStringUtil::serializeForCatalog(nss),
                                                  config,
                                                  kIsLogged);
        ASSERT_OK(result.getStatus());

        auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
        std::string uri = "table:" + ns;
        invariant(Status::OK() ==
                  WiredTigerIndex::create(WiredTigerRecoveryUnit::get(ru), uri, result.getValue()));

        return std::make_unique<WiredTigerIdIndex>(
            opCtx, ru, uri, UUID::gen(), "" /* ident */, config, kIsLogged);
    }

    std::unique_ptr<SortedDataInterface> newSortedDataInterface(OperationContext* opCtx,
                                                                bool unique,
                                                                bool partial,
                                                                KeyFormat keyFormat) final {
        std::string ns = "test.wt";
        NamespaceString nss = NamespaceString::createNamespaceString_forTest(ns);
        std::string indexName = "textIndex";
        BSONObj spec =
            BSON("key" << BSON("a" << 1) << "name" << indexName << "v"
                       << static_cast<int>(IndexConfig::kLatestIndexVersion) << "unique" << unique);

        if (partial) {
            auto partialBSON = BSON("partialFilterExpression" << BSON("" << ""));
            spec = spec.addField(partialBSON.firstElement());
        }

        auto ordering = Ordering::allAscending();
        IndexConfig config{false /* isIdIndex */,
                           unique,
                           IndexConfig::kLatestIndexVersion,
                           spec,
                           indexName,
                           ordering};
        StatusWith<std::string> result =
            WiredTigerIndex::generateCreateString(std::string{kWiredTigerEngineName},
                                                  "",
                                                  "",
                                                  NamespaceStringUtil::serializeForCatalog(nss),
                                                  config,
                                                  kIsLogged);
        ASSERT_OK(result.getStatus());

        auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
        std::string uri = "table:" + ns;
        invariant(Status::OK() ==
                  WiredTigerIndex::create(WiredTigerRecoveryUnit::get(ru), uri, result.getValue()));

        if (unique) {
            return std::make_unique<WiredTigerIndexUnique>(
                opCtx, ru, uri, UUID::gen(), "" /* ident */, keyFormat, config, kIsLogged);
        }
        return std::make_unique<WiredTigerIndexStandard>(
            opCtx, ru, uri, UUID::gen(), "" /* ident */, keyFormat, config, kIsLogged);
    }

    std::unique_ptr<RecoveryUnit> newRecoveryUnit() final {
        return std::make_unique<WiredTigerRecoveryUnit>(_connection.get(), nullptr);
    }

private:
    unittest::TempDir _dbpath;
    std::unique_ptr<ClockSource> _fastClockSource;
    WT_CONNECTION* _conn;
    std::unique_ptr<WiredTigerConnection> _connection;
};

MONGO_INITIALIZER(RegisterSortedDataInterfaceHarnessFactory)(InitializerContext* const) {
    registerSortedDataInterfaceHarnessHelperFactory([](int32_t cacheSizeMB) {
        return std::make_unique<WiredTigerIndexHarnessHelper>(cacheSizeMB);
    });
}

}  // namespace
}  // namespace mongo
