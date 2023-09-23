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

#include <boost/move/utility_core.hpp>
#include <memory>
#include <string>
#include <vector>
#include <wiredtiger.h>

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_oplog_manager.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

class WiredTigerIndexHarnessHelper final : public SortedDataInterfaceHarnessHelper {
public:
    WiredTigerIndexHarnessHelper() : _dbpath("wt_test"), _conn(nullptr) {
        const char* config = "create,cache_size=1G,";
        int ret = wiredtiger_open(_dbpath.path().c_str(), nullptr, config, &_conn);
        invariantWTOK(ret, nullptr);

        _fastClockSource = std::make_unique<SystemClockSource>();
        _sessionCache = new WiredTigerSessionCache(_conn, _fastClockSource.get());
    }

    ~WiredTigerIndexHarnessHelper() final {
        delete _sessionCache;
        _conn->close(_conn, nullptr);
    }

    std::unique_ptr<SortedDataInterface> newIdIndexSortedDataInterface() final {
        std::string ns = "test.wt";
        NamespaceString nss = NamespaceString::createNamespaceString_forTest(ns);
        auto opCtxHolder{newOperationContext()};
        auto* const opCtx{opCtxHolder.get()};

        BSONObj spec = BSON("key" << BSON("_id" << 1) << "name"
                                  << "_id_"
                                  << "v" << static_cast<int>(IndexDescriptor::kLatestIndexVersion)
                                  << "unique" << true);

        auto collection = std::make_unique<CollectionMock>(nss);
        IndexDescriptor desc("", spec);
        invariant(desc.isIdIndex());

        const bool isLogged = false;
        StatusWith<std::string> result = WiredTigerIndex::generateCreateString(
            kWiredTigerEngineName, "", "", nss, desc, isLogged);
        ASSERT_OK(result.getStatus());

        std::string uri = "table:" + ns;
        invariant(Status::OK() == WiredTigerIndex::create(opCtx, uri, result.getValue()));

        return std::make_unique<WiredTigerIdIndex>(
            opCtx, uri, UUID::gen(), "" /* ident */, &desc, isLogged);
    }

    std::unique_ptr<SortedDataInterface> newSortedDataInterface(bool unique,
                                                                bool partial,
                                                                KeyFormat keyFormat) final {
        std::string ns = "test.wt";
        NamespaceString nss = NamespaceString::createNamespaceString_forTest(ns);
        auto opCtxHolder{newOperationContext()};
        auto* const opCtx{opCtxHolder.get()};

        BSONObj spec = BSON("key" << BSON("a" << 1) << "name"
                                  << "testIndex"
                                  << "v" << static_cast<int>(IndexDescriptor::kLatestIndexVersion)
                                  << "unique" << unique);

        if (partial) {
            auto partialBSON =
                BSON(IndexDescriptor::kPartialFilterExprFieldName.toString() << BSON(""
                                                                                     << ""));
            spec = spec.addField(partialBSON.firstElement());
        }

        auto collection = std::make_unique<CollectionMock>(nss);

        IndexDescriptor& desc = _descriptors.emplace_back("", spec);

        StatusWith<std::string> result = WiredTigerIndex::generateCreateString(
            kWiredTigerEngineName, "", "", nss, desc, WiredTigerUtil::useTableLogging(nss));
        ASSERT_OK(result.getStatus());

        std::string uri = "table:" + ns;
        invariant(Status::OK() == WiredTigerIndex::create(opCtx, uri, result.getValue()));

        if (unique) {
            return std::make_unique<WiredTigerIndexUnique>(opCtx,
                                                           uri,
                                                           UUID::gen(),
                                                           "" /* ident */,
                                                           keyFormat,
                                                           &desc,
                                                           WiredTigerUtil::useTableLogging(nss));
        }
        return std::make_unique<WiredTigerIndexStandard>(opCtx,
                                                         uri,
                                                         UUID::gen(),
                                                         "" /* ident */,
                                                         keyFormat,
                                                         &desc,
                                                         WiredTigerUtil::useTableLogging(nss));
    }

    std::unique_ptr<RecoveryUnit> newRecoveryUnit() final {
        return std::make_unique<WiredTigerRecoveryUnit>(_sessionCache, &_oplogManager);
    }

private:
    unittest::TempDir _dbpath;
    std::unique_ptr<ClockSource> _fastClockSource;
    std::vector<IndexDescriptor> _descriptors;
    WT_CONNECTION* _conn;
    WiredTigerSessionCache* _sessionCache;
    WiredTigerOplogManager _oplogManager;
};

MONGO_INITIALIZER(RegisterSortedDataInterfaceHarnessFactory)(InitializerContext* const) {
    registerSortedDataInterfaceHarnessHelperFactory(
        [] { return std::make_unique<WiredTigerIndexHarnessHelper>(); });
}

}  // namespace
}  // namespace mongo
