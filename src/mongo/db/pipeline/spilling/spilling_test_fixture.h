// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Test fixtures that sets up a process interface to allow spilling.
 */
class SpillingTestFixture : public ServiceContextMongoDTest {
public:
    SpillingTestFixture();

protected:
    ServiceContext::UniqueOperationContext _opCtx;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

}  // namespace mongo
