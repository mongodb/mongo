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

#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_btree_impl.h"

#include <memory>

#include "mongo/base/init.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_recovery_unit.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class EphemeralForTestBtreeImplTestHarnessHelper final
    : public virtual SortedDataInterfaceHarnessHelper {
public:
    EphemeralForTestBtreeImplTestHarnessHelper() : _order(Ordering::make(BSONObj())) {}

    std::unique_ptr<SortedDataInterface> newIdIndexSortedDataInterface() final {
        BSONObj spec = BSON("key" << BSON("_id" << 1) << "name"
                                  << "_id_"
                                  << "v" << static_cast<int>(IndexDescriptor::kLatestIndexVersion)
                                  << "unique" << true);

        return std::unique_ptr<SortedDataInterface>(
            getEphemeralForTestBtreeImpl(_order,
                                         true /* unique */,
                                         NamespaceString("test.EphemeralForTest"),
                                         "indexName",
                                         spec,
                                         BSONObj{},
                                         &_data));
    }

    std::unique_ptr<SortedDataInterface> newSortedDataInterface(bool unique, bool partial) final {
        return std::unique_ptr<SortedDataInterface>(
            getEphemeralForTestBtreeImpl(_order,
                                         unique,
                                         NamespaceString("test.EphemeralForTest"),
                                         "indexName",
                                         BSONObj{},
                                         BSONObj{},
                                         &_data));
    }

    std::unique_ptr<RecoveryUnit> newRecoveryUnit() final {
        return std::make_unique<EphemeralForTestRecoveryUnit>();
    }

private:
    std::shared_ptr<void> _data;  // used by EphemeralForTestBtreeImpl
    Ordering _order;
};

std::unique_ptr<SortedDataInterfaceHarnessHelper> makeEFTHarnessHelper() {
    return std::make_unique<EphemeralForTestBtreeImplTestHarnessHelper>();
}

MONGO_INITIALIZER(RegisterSortedDataInterfaceHarnessFactory)(InitializerContext* const) {
    mongo::registerSortedDataInterfaceHarnessHelperFactory(makeEFTHarnessHelper);
    return Status::OK();
}
}  // namespace
}  // namespace mongo
