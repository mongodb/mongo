/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/write_concern.h"

#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/storage_engine_mock.h"
#include "mongo/db/write_concern_idl.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {
// Need to use non-ephemeral engine to be able to test journaling options.
class StorageEngineNonEphemeralMock : public StorageEngineMock {
    bool isEphemeral() const override {
        return false;
    }
};

class WriteConcernEphemeralTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        auto* service = getServiceContext();
        setupStorageEngine();
        _opCtx = cc().makeOperationContext();
        auto mockReplCoord =
            std::make_unique<repl::ReplicationCoordinatorMock>(service, createReplSettings());
        _mockReplCoord = mockReplCoord.get();
        repl::ReplicationCoordinator::set(service, std::move(mockReplCoord));
    }

    virtual void setupStorageEngine() {
        auto* service = getServiceContext();
        service->setStorageEngine(std::make_unique<StorageEngineMock>());
    }

protected:
    repl::ReplicationCoordinatorMock* _mockReplCoord;
    ServiceContext::UniqueOperationContext _opCtx;

private:
    virtual repl::ReplSettings createReplSettings() {
        repl::ReplSettings settings;
        settings.setReplSetString("mySet/node1:12345");
        return settings;
    }
};

class WriteConcernTest : public WriteConcernEphemeralTest {
    void setupStorageEngine() override {
        auto* service = getServiceContext();
        service->setStorageEngine(std::make_unique<StorageEngineNonEphemeralMock>());
    }
};

GenericArguments makeGenericArgs(const WriteConcernIdl& wc) {
    GenericArguments args;
    args.setWriteConcern(uassertStatusOK(WriteConcernOptions::parse(wc.toBSON())));
    return args;
}

constexpr auto kCommandName = "doSomeWrite"_sd;

TEST_F(WriteConcernTest, ParseFailsOnNullBytes) {
    std::string s = "wcWithNullBytes ";
    s[s.length() - 1] = '\0';
    auto testDoc = BSON("w" << s);
    ASSERT_THROWS_CODE_AND_WHAT(
        WriteConcernIdl::parse(testDoc, IDLParserContext("writeConcernTest")),
        DBException,
        ErrorCodes::FailedToParse,
        "w has illegal embedded NUL byte, w: wcWithNullBytes");
}

TEST_F(WriteConcernTest, ExtractOverridesWMajorityJFalse) {
    WriteConcernIdl wc;
    wc.setWriteConcernW(WriteConcernW("majority"));
    wc.setJ(false);
    auto swWriteConcernOptions =
        extractWriteConcern(_opCtx.get(), makeGenericArgs(wc), kCommandName, false /*internal*/);
    auto writeConcernOptions = unittest::assertGet(swWriteConcernOptions);
    ASSERT(writeConcernOptions.isMajority());
    ASSERT_EQ(writeConcernOptions.syncMode, WriteConcernOptions::SyncMode::JOURNAL);
    ASSERT(writeConcernOptions.majorityJFalseOverridden);
}

TEST_F(WriteConcernTest,
       ExtractDoesNotOverrideWMajorityJFalseWhenWriteConcernMajorityDefaultIsFalse) {
    WriteConcernIdl wc;
    wc.setWriteConcernW(WriteConcernW("majority"));
    wc.setJ(false);
    _mockReplCoord->setWriteConcernMajorityShouldJournal(false);
    auto swWriteConcernOptions =
        extractWriteConcern(_opCtx.get(), makeGenericArgs(wc), kCommandName, false /*internal*/);
    auto writeConcernOptions = unittest::assertGet(swWriteConcernOptions);
    ASSERT(writeConcernOptions.isMajority());
    ASSERT_EQ(writeConcernOptions.syncMode, WriteConcernOptions::SyncMode::NONE);
    ASSERT_FALSE(writeConcernOptions.majorityJFalseOverridden);
}

TEST_F(WriteConcernTest, ExtractDoesNotOverrideW1JFalse) {
    WriteConcernIdl wc;
    wc.setWriteConcernW(WriteConcernW(1));
    wc.setJ(false);
    auto swWriteConcernOptions =
        extractWriteConcern(_opCtx.get(), makeGenericArgs(wc), kCommandName, false /*internal*/);
    auto writeConcernOptions = unittest::assertGet(swWriteConcernOptions);
    ASSERT_EQ(writeConcernOptions.w, WriteConcernW{1});
    ASSERT_EQ(writeConcernOptions.syncMode, WriteConcernOptions::SyncMode::NONE);
    ASSERT_FALSE(writeConcernOptions.majorityJFalseOverridden);
}

TEST_F(WriteConcernTest, ExtractDoesNotOverrideWMajorityJUnset) {
    WriteConcernIdl wc;
    wc.setWriteConcernW(WriteConcernW("majority"));
    auto swWriteConcernOptions =
        extractWriteConcern(_opCtx.get(), makeGenericArgs(wc), kCommandName, false /*internal*/);
    auto writeConcernOptions = unittest::assertGet(swWriteConcernOptions);
    ASSERT(writeConcernOptions.isMajority());
    ASSERT_EQ(writeConcernOptions.syncMode, WriteConcernOptions::SyncMode::UNSET);
    ASSERT_FALSE(writeConcernOptions.majorityJFalseOverridden);
}

TEST_F(WriteConcernTest, ExtractDoesNotOverrideWMajorityJTrue) {
    WriteConcernIdl wc;
    wc.setWriteConcernW(WriteConcernW("majority"));
    wc.setJ(true);
    auto swWriteConcernOptions =
        extractWriteConcern(_opCtx.get(), makeGenericArgs(wc), kCommandName, false /*internal*/);
    auto writeConcernOptions = unittest::assertGet(swWriteConcernOptions);
    ASSERT(writeConcernOptions.isMajority());
    ASSERT_EQ(writeConcernOptions.syncMode, WriteConcernOptions::SyncMode::JOURNAL);
    ASSERT_FALSE(writeConcernOptions.majorityJFalseOverridden);
}

TEST_F(WriteConcernEphemeralTest, ExtractDoesNotOverrideWMajorityJFalseOnEphemeral) {
    // If we override {"w" : "majority", "j" : false } to {"j" : true} on an ephemeral storage
    // engine, the command will fail.  So make sure we don't do that.
    WriteConcernIdl wc;
    wc.setWriteConcernW(WriteConcernW("majority"));
    wc.setJ(false);
    auto swWriteConcernOptions =
        extractWriteConcern(_opCtx.get(), makeGenericArgs(wc), kCommandName, false /*internal*/);
    auto writeConcernOptions = unittest::assertGet(swWriteConcernOptions);
    ASSERT(writeConcernOptions.isMajority());
    ASSERT_EQ(writeConcernOptions.syncMode, WriteConcernOptions::SyncMode::NONE);
    ASSERT_FALSE(writeConcernOptions.majorityJFalseOverridden);
}

}  // namespace
}  // namespace mongo
