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

#include "mongo/db/query/write_ops/insert.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>

namespace mongo {
namespace {
const auto kInsertTestNss = NamespaceString::createNamespaceString_forTest("dbtests.InsertTest");

class InsertTest : public unittest::Test {
public:
    InsertTest()
        : _opCtx(cc().makeOperationContext()),
          _lock(_opCtx.get()),
          _autoColl(_opCtx.get(), kInsertTestNss, MODE_IX) {}

    OperationContext* getOperationContext() const {
        return _opCtx.get();
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    Lock::GlobalWrite _lock;
    AutoGetCollection _autoColl;
};

BSONObj makeNestedObject(size_t depth) {
    ASSERT_GTE(depth, 1U);

    auto obj = BSON("a" << 1);
    while (--depth) {
        obj = BSON("a" << obj);
    }

    return obj;
}

BSONArray makeNestedArray(size_t depth) {
    ASSERT_GTE(depth, 1U);

    auto obj = BSON_ARRAY(1);
    while (--depth) {
        obj = BSON_ARRAY(obj);
    }

    return obj;
}

TEST_F(InsertTest, FixDocumentForInsertAcceptsEmptyDocuments) {
    ASSERT_OK(fixDocumentForInsert(getOperationContext(), BSONObj()));
}

TEST_F(InsertTest, FixDocumentForInsertAcceptsDocumentsAtStorageDepthLimit) {
    ASSERT_OK(fixDocumentForInsert(getOperationContext(),
                                   makeNestedObject(BSONDepth::getMaxDepthForUserStorage())));
    ASSERT_OK(fixDocumentForInsert(getOperationContext(),
                                   makeNestedArray(BSONDepth::getMaxDepthForUserStorage())));
}

TEST_F(InsertTest, FixDocumentForInsertFailsOnDeeplyNestedDocuments) {
    ASSERT_EQ(fixDocumentForInsert(getOperationContext(),
                                   makeNestedObject(BSONDepth::getMaxDepthForUserStorage() + 1)),
              ErrorCodes::Overflow);
    ASSERT_EQ(fixDocumentForInsert(getOperationContext(),
                                   makeNestedArray(BSONDepth::getMaxDepthForUserStorage() + 1)),
              ErrorCodes::Overflow);
}
}  // namespace
}  // namespace mongo
