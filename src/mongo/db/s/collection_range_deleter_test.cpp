/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/s/collection_range_deleter.h"

#include <algorithm>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/write_concern.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {

using unittest::assertGet;
class ChunkRange;

namespace {

const NamespaceString nss = NamespaceString("TestDB", "CollDB");

TEST_F(ServiceContextMongoDTest, EmptyRangesToClean) {
    auto serviceContext = getServiceContext();
    MetadataManager manager(serviceContext, nss);
    CollectionRangeDeleter* rangeDeleter = new CollectionRangeDeleter(nss);

    ASSERT(!rangeDeleter->cleanupNextRange(cc().makeOperationContext().get()));

    delete rangeDeleter;
}

TEST_F(ServiceContextMongoDTest, NotEmptyRangesToClean) {
    auto serviceContext = getServiceContext();
    MetadataManager manager(serviceContext, nss);
    ChunkRange cr1 = ChunkRange(BSON("key" << 0), BSON("key" << 10));
    auto status = manager.addRangeToClean(cr1);

    CollectionRangeDeleter* rangeDeleter = new CollectionRangeDeleter(nss);

    ASSERT(!rangeDeleter->cleanupNextRange(cc().makeOperationContext().get()));

    delete rangeDeleter;
}

// TODO: tests that need to be built

// Add data to database, add range NOT in database -> nothing to clean
// Add data to database, add range IN database -> delete single document
// Delete single document, then confirm nothing left to clean
// Delete single document, then confirm something left to clean
// Add more documents than MAX, then delete until nothing left to clean



}  // unnamed namespace

} // namespace mongo
