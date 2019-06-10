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

#include "mongo/db/storage/biggie/biggie_sorted_impl.h"

#include <memory>

#include "mongo/base/init.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/biggie/biggie_kv_engine.h"
#include "mongo/db/storage/biggie/biggie_recovery_unit.h"
#include "mongo/db/storage/biggie/store.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace biggie {
namespace {
class SortedDataInterfaceTestHarnessHelper final : public virtual SortedDataInterfaceHarnessHelper {
private:
    KVEngine _kvEngine{};
    Ordering _order;

public:
    SortedDataInterfaceTestHarnessHelper() : _order(Ordering::make(BSONObj())) {}
    std::unique_ptr<mongo::SortedDataInterface> newSortedDataInterface(bool unique,
                                                                       bool partial) final {
        std::string ns = "test.biggie";
        OperationContextNoop opCtx(newRecoveryUnit().release());

        BSONObj spec = BSON("key" << BSON("a" << 1) << "name"
                                  << "testIndex"
                                  << "v"
                                  << static_cast<int>(IndexDescriptor::kLatestIndexVersion)
                                  << "ns"
                                  << ns
                                  << "unique"
                                  << unique);
        if (partial) {
            auto partialBSON =
                BSON(IndexDescriptor::kPartialFilterExprFieldName.toString() << BSON(""
                                                                                     << ""));
            spec = spec.addField(partialBSON.firstElement());
        }

        IndexDescriptor desc(NULL, "", spec);

        return std::make_unique<SortedDataInterface>(&opCtx, "ident"_sd, &desc);
    }

    std::unique_ptr<mongo::RecoveryUnit> newRecoveryUnit() final {
        return std::make_unique<RecoveryUnit>(&_kvEngine);
    }
};

std::unique_ptr<HarnessHelper> makeHarnessHelper() {
    return std::make_unique<SortedDataInterfaceTestHarnessHelper>();
}

MONGO_INITIALIZER(RegisterHarnessFactory)(InitializerContext* const) {
    mongo::registerHarnessHelperFactory(makeHarnessHelper);
    return Status::OK();
}
}  // namespace
}  // namespace biggie
}  // namespace mongo
