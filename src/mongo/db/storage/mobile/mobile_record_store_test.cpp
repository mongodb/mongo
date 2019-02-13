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

#include "mongo/platform/basic.h"

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include "mongo/base/init.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/storage/mobile/mobile_record_store.h"
#include "mongo/db/storage/mobile/mobile_recovery_unit.h"
#include "mongo/db/storage/mobile/mobile_session.h"
#include "mongo/db/storage/mobile/mobile_session_pool.h"
#include "mongo/db/storage/mobile/mobile_sqlite_statement.h"
#include "mongo/db/storage/mobile/mobile_util.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"


namespace mongo {

namespace {

static int inc = 0;

class MobileHarnessHelper final : public RecordStoreHarnessHelper {
public:
    MobileHarnessHelper() : _dbPath("mobile_record_store_harness") {
        // TODO: Determine if this should be util function.
        boost::system::error_code err;
        boost::filesystem::path dir(_dbPath.path());

        if (!boost::filesystem::exists(dir, err)) {
            if (err) {
                uasserted(ErrorCodes::UnknownError, err.message());
            }

            boost::filesystem::create_directory(dir, err);
            if (err) {
                uasserted(ErrorCodes::UnknownError, err.message());
            }
        }

        boost::filesystem::path file("mobile.sqlite");
        boost::filesystem::path fullPath = dir / file;

        if (boost::filesystem::exists(fullPath, err)) {
            if (err) {
                uasserted(ErrorCodes::UnknownError, err.message());
            } else if (!boost::filesystem::is_regular_file(fullPath)) {
                std::string errMsg("Failed to open " + dir.generic_string() +
                                   ": not a regular file");
                uasserted(ErrorCodes::BadValue, errMsg);
            }
        }

        _fullPath = fullPath.string();
        _sessionPool.reset(new MobileSessionPool(_fullPath));
    }

    std::unique_ptr<RecordStore> newNonCappedRecordStore() override {
        inc++;
        return newNonCappedRecordStore("table_" + std::to_string(inc));
    }

    std::unique_ptr<RecordStore> newNonCappedRecordStore(const std::string& ns) override {
        ServiceContext::UniqueOperationContext opCtx(this->newOperationContext());
        MobileRecordStore::create(opCtx.get(), ns);
        return stdx::make_unique<MobileRecordStore>(
            opCtx.get(), ns, _fullPath, ns, CollectionOptions());
    }

    std::unique_ptr<RecordStore> newCappedRecordStore(int64_t cappedMaxSize,
                                                      int64_t cappedMaxDocs) override {
        inc++;
        return newCappedRecordStore("table_" + std::to_string(inc), cappedMaxSize, cappedMaxDocs);
    }

    std::unique_ptr<RecordStore> newCappedRecordStore(const std::string& ns,
                                                      int64_t cappedMaxSize,
                                                      int64_t cappedMaxDocs) override {
        ServiceContext::UniqueOperationContext opCtx(this->newOperationContext());
        MobileRecordStore::create(opCtx.get(), ns);
        CollectionOptions options;
        options.capped = true;
        options.cappedSize = cappedMaxSize;
        options.cappedMaxDocs = cappedMaxDocs;
        return stdx::make_unique<MobileRecordStore>(opCtx.get(), ns, _fullPath, ns, options);
    }

    std::unique_ptr<RecoveryUnit> newRecoveryUnit() final {
        return stdx::make_unique<MobileRecoveryUnit>(_sessionPool.get());
    }

    bool supportsDocLocking() final {
        return false;
    }

private:
    unittest::TempDir _dbPath;
    std::string _fullPath;
    std::unique_ptr<MobileSessionPool> _sessionPool;
};

std::unique_ptr<HarnessHelper> makeHarnessHelper() {
    return stdx::make_unique<MobileHarnessHelper>();
}

MONGO_INITIALIZER(RegisterHarnessFactory)(InitializerContext* const) {
    mongo::registerHarnessHelperFactory(makeHarnessHelper);
    return Status::OK();
}
}  // namespace
}  // namespace mongo
