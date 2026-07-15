// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class RawDataOperationTest : public ServiceContextTest {
public:
    ServiceContext::UniqueOperationContext opCtxHolder{makeOperationContext()};
    OperationContext* opCtx{opCtxHolder.get()};
};

TEST_F(RawDataOperationTest, ScopedGuardSetsAndRestoresValue) {
    ASSERT_FALSE(isRawDataOperation(opCtx));
    {
        ScopedRawDataOperation guard(opCtx, true);
        ASSERT_TRUE(isRawDataOperation(opCtx));
    }
    ASSERT_FALSE(isRawDataOperation(opCtx));
}

TEST_F(RawDataOperationTest, ScopedGuardRestoresOriginalTrueValue) {
    isRawDataOperation(opCtx) = true;
    {
        ScopedRawDataOperation guard(opCtx, false);
        ASSERT_FALSE(isRawDataOperation(opCtx));
    }
    ASSERT_TRUE(isRawDataOperation(opCtx));
}

TEST_F(RawDataOperationTest, ScopedGuardsNest) {
    ASSERT_FALSE(isRawDataOperation(opCtx));
    {
        ScopedRawDataOperation outer(opCtx, true);
        ASSERT_TRUE(isRawDataOperation(opCtx));
        {
            ScopedRawDataOperation inner(opCtx, false);
            ASSERT_FALSE(isRawDataOperation(opCtx));
        }
        ASSERT_TRUE(isRawDataOperation(opCtx));
    }
    ASSERT_FALSE(isRawDataOperation(opCtx));
}

}  // namespace
}  // namespace mongo
