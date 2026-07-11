// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/util/modules.h"

#include <memory>
#include <utility>

namespace mongo::stats {

/**
 * Sets up and provides a repl::StorageInterface and OperationContext.
 * Database data are cleared  between test runs.
 */
class StatsCacheLoaderTestFixture : public ServiceContextMongoDTest {
public:
    explicit StatsCacheLoaderTestFixture(Options options = {})
        : ServiceContextMongoDTest(std::move(options)) {}

    OperationContext* operationContext();
    repl::StorageInterface* storageInterface();

protected:
    void setUp() override;
    void tearDown() override;

private:
    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<repl::StorageInterface> _storage;
};

}  // namespace mongo::stats
