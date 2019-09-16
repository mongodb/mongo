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

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <memory>

#include "mongo/base/init.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/mobile/mobile_index.h"
#include "mongo/db/storage/mobile/mobile_recovery_unit.h"
#include "mongo/db/storage/mobile/mobile_session_pool.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/platform/basic.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
int inc = 0;

class MobileIndexTestHarnessHelper final : public virtual SortedDataInterfaceHarnessHelper {
public:
    MobileIndexTestHarnessHelper()
        : _dbPath("mobile_index_harness"), _ordering(Ordering::make(BSONObj())) {
        boost::filesystem::path fullPath(_dbPath.path());
        fullPath /= "mobile.sqlite";
        _fullPath = fullPath.string();
        _sessionPool.reset(new MobileSessionPool(_fullPath));
    }

    std::unique_ptr<SortedDataInterface> newIdIndexSortedDataInterface() final {
        return newSortedDataInterface(true /* unique */, false /* partial */);
    }

    std::unique_ptr<SortedDataInterface> newSortedDataInterface(bool isUnique, bool isPartial) {
        std::string ident("index_" + std::to_string(inc++));
        OperationContextNoop opCtx(newRecoveryUnit().release());
        Status status = MobileIndex::create(&opCtx, ident);
        fassert(37052, status);

        if (isUnique) {
            return std::make_unique<MobileIndexUnique>(
                _ordering, ident, "test.mobile", "indexName");
        }
        return std::make_unique<MobileIndexStandard>(_ordering, ident, "test.mobile", "indexName");
    }

    std::unique_ptr<RecoveryUnit> newRecoveryUnit() {
        return std::make_unique<MobileRecoveryUnit>(_sessionPool.get());
    }

private:
    unittest::TempDir _dbPath;
    std::string _fullPath;
    std::unique_ptr<MobileSessionPool> _sessionPool;
    const Ordering _ordering;
};

std::unique_ptr<SortedDataInterfaceHarnessHelper> makeMobileIndexHarnessHelper() {
    return std::make_unique<MobileIndexTestHarnessHelper>();
}

MONGO_INITIALIZER(RegisterSortedDataInterfaceHarnessFactory)(InitializerContext* const) {
    mongo::registerSortedDataInterfaceHarnessHelperFactory(makeMobileIndexHarnessHelper);
    return Status::OK();
}
}  // namespace mongo
