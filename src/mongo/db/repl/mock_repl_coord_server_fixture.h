// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/modules.h"

#include <utility>

namespace mongo {

class OperationContext;

namespace repl {
class OplogEntry;
class StorageInterfaceMock;
}  // namespace repl

/**
 * This is a basic fixture that is backed by a real storage engine and a mock replication
 * coordinator that is running as primary.
 */
class [[MONGO_MOD_OPEN]] MockReplCoordServerFixture : public ServiceContextMongoDTest {
public:
    void setUp() override;

    /**
     * Helper method for inserting new entries to the oplog. This completely bypasses
     * fixDocumentForInsert.
     */
    void insertOplogEntry(const repl::OplogEntry& entry);

    OperationContext* opCtx();

protected:
    explicit MockReplCoordServerFixture(Options options = {})
        : ServiceContextMongoDTest(std::move(options)) {}

private:
    ServiceContext::UniqueOperationContext _opCtx;
    repl::StorageInterfaceMock* _storageInterface;
};

}  // namespace mongo
