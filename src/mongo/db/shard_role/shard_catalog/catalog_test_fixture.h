// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/util/modules.h"

#include <utility>

namespace mongo {

/**
 * General test fixture for storage catalog tests.
 *
 * Sets up and provides a repl::StorageInterface and OperationContext. Database data are cleared
 * between test runs.
 */
class [[MONGO_MOD_PUBLIC]] CatalogScopedGlobalServiceContextForTest
    : public MongoDScopedGlobalServiceContextForTest {
public:
    CatalogScopedGlobalServiceContextForTest(Options options, bool shouldSetupTL);
};

class [[MONGO_MOD_OPEN]] CatalogTestFixture : public ServiceContextTest {
public:
    using Options = MongoDScopedGlobalServiceContextForTest::Options;

    CatalogTestFixture() : CatalogTestFixture(Options{}) {}

    explicit CatalogTestFixture(Options options)
        : ServiceContextTest(std::make_unique<CatalogScopedGlobalServiceContextForTest>(
              std::move(options), shouldSetupTL)) {}

    OperationContext* operationContext() const;

    void setUp() override;
    void tearDown() override;

    repl::StorageInterface* storageInterface() const;

    ConsistentCollection makeConsistentCollection(const Collection*) const;
    ConsistentCollection makeConsistentCollection(OperationContext* opCtx, const Collection*) const;
    int getReferenceCount(const ConsistentCollection& coll) const;

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

}  // namespace mongo
