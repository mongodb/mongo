// btree_interface_test.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/storage/mmap_v1/btree/btree_interface.h"
#include "mongo/db/storage/mmap_v1/btree/btree_test_help.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using std::unique_ptr;

class MyHarnessHelper final : public HarnessHelper {
public:
    MyHarnessHelper() : _recordStore("a.b"), _order(Ordering::make(BSONObj())) {}

    std::unique_ptr<SortedDataInterface> newSortedDataInterface(bool unique) final {
        std::unique_ptr<SortedDataInterface> sorted(getMMAPV1Interface(&_headManager,
                                                                       &_recordStore,
                                                                       &_cursorRegistry,
                                                                       _order,
                                                                       "a_1",  // indexName
                                                                       1,      // version
                                                                       unique));
        OperationContextNoop op;
        massertStatusOK(sorted->initAsEmpty(&op));
        return sorted;
    }

    std::unique_ptr<RecoveryUnit> newRecoveryUnit() final {
        return stdx::make_unique<HeapRecordStoreBtreeRecoveryUnit>();
    }

private:
    TestHeadManager _headManager;
    HeapRecordStoreBtree _recordStore;
    SavedCursorRegistry _cursorRegistry;
    Ordering _order;
};

std::unique_ptr<HarnessHelper> newHarnessHelper() {
    return stdx::make_unique<MyHarnessHelper>();
}
}
