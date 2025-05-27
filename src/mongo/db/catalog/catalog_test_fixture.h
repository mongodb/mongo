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

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"

#include <utility>

namespace mongo {

/**
 * General test fixture for storage catalog tests.
 *
 * Sets up and provides a repl::StorageInterface and OperationContext. Database data are cleared
 * between test runs.
 */
class CatalogScopedGlobalServiceContextForTest : public MongoDScopedGlobalServiceContextForTest {
public:
    CatalogScopedGlobalServiceContextForTest(Options options, bool shouldSetupTL);
};

class CatalogTestFixture : public ServiceContextTest {
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
