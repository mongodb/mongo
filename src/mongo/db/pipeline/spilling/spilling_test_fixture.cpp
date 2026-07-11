// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/spilling/spilling_test_fixture.h"

#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/process_interface/standalone_process_interface.h"

namespace mongo {

// This fixture tests spilling behavior directly, requiring the spill WiredTiger instance.
SpillingTestFixture::SpillingTestFixture()
    : ServiceContextMongoDTest(Options{}.enableSpillEngine()),
      _opCtx(makeOperationContext()),
      _expCtx(new ExpressionContextForTest(_opCtx.get(),
                                           NamespaceString::createNamespaceString_forTest(
                                               "SpillingTestFixture.SpillingTestFixture"))) {
    _expCtx->setMongoProcessInterface(std::make_shared<StandaloneProcessInterface>(nullptr));
}

}  // namespace mongo
