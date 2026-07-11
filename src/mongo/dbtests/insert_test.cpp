// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/write_ops/insert.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
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
