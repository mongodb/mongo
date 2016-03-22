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

#include "mongo/base/init.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/collation/collator_factory_mock.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

// Stub to avoid including the server environment library.
MONGO_INITIALIZER(SetGlobalEnvironment)(InitializerContext* context) {
    setGlobalServiceContext(stdx::make_unique<ServiceContextNoop>());
    return Status::OK();
}

TEST(CollatorFactoryMockTest, CollatorFactoryMockConstructsReverseStringCollator) {
    CollatorFactoryMock factory;
    auto collator = factory.makeFromBSON(BSONObj());
    ASSERT_OK(collator.getStatus());
    ASSERT_GT(collator.getValue()->compare("abc", "cba"), 0);
}

// TODO SERVER-22371: Remove this test. We will decorate with a CollatorFactoryICU instead.
TEST(CollatorFactoryMockTest, ServiceContextDecoratedWithCollatorFactoryMock) {
    auto factory = CollatorFactoryInterface::get(getGlobalServiceContext());
    auto collator = factory->makeFromBSON(BSONObj());
    ASSERT_OK(collator.getStatus());
    ASSERT_GT(collator.getValue()->compare("abc", "cba"), 0);
}

}  // namespace
