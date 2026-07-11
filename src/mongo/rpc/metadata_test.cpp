// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/metadata.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/json.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/shared_buffer.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace {
using namespace mongo;
using namespace mongo::rpc;

BSONObj addDollarDB(BSONObj command, std::string_view db) {
    return BSONObjBuilder(std::move(command)).append("$db", db).obj();
}

void checkUpconvert(const BSONObj& legacyCommand,
                    const int legacyQueryFlags,
                    BSONObj upconvertedCommand) {
    upconvertedCommand = addDollarDB(std::move(upconvertedCommand), "db");
    auto converted = upconvertRequest(DatabaseName::createDatabaseName_forTest(boost::none, "db"),
                                      legacyCommand,
                                      legacyQueryFlags);

    // We don't care about the order of the fields in the metadata object
    const auto sorted = [](const BSONObj& obj) {
        BSONObjIteratorSorted iter(obj);
        BSONObjBuilder bob;
        while (iter.more()) {
            bob.append(iter.next());
        }
        return bob.obj();
    };

    ASSERT_BSONOBJ_EQ(sorted(upconvertedCommand), sorted(converted.body));
}

TEST(Metadata, UpconvertValidMetadata) {
    // Unwrapped, no readPref, no slaveOk
    checkUpconvert(BSON("ping" << 1), 0, BSON("ping" << 1));

    // Readpref wrapped in $queryOptions
    checkUpconvert(
        BSON("pang" << "pong"
                    << "$queryOptions"
                    << BSON("$readPreference" << BSON("mode" << "nearest"
                                                             << "tags" << BSON("rack" << "city")))),
        0,
        BSON("pang" << "pong"
                    << "$readPreference"
                    << BSON("mode" << "nearest"
                                   << "tags" << BSON("rack" << "city"))));
}

TEST(Metadata, UpconvertDuplicateReadPreference) {
    auto secondaryReadPref = BSON("mode" << "secondary");
    auto nearestReadPref = BSON("mode" << "nearest");

    BSONObjBuilder bob;
    bob.append("$queryOptions", BSON("$readPreference" << secondaryReadPref));
    bob.append("$readPreference", nearestReadPref);

    ASSERT_THROWS_CODE(
        rpc::upconvertRequest(
            DatabaseName::createDatabaseName_forTest(boost::none, "db"), bob.obj(), 0),
        AssertionException,
        ErrorCodes::InvalidOptions);
}

TEST(Metadata, UpconvertOnlyExtractsReadPreference) {
    const auto dbName = DatabaseName::createDatabaseName_forTest(boost::none, "db");
    auto legacyCmdObj = fromjson(
        "{aggregate: 'someCollection', $queryOptions: {someRandomField: 'Hello, world!'}}");
    auto expectedCmdObj = fromjson("{aggregate: 'someCollection'}");
    checkUpconvert(legacyCmdObj, /* queryFlags */ 0, expectedCmdObj);
}

TEST(Metadata, UpconvertUsesDocumentSequecesCorrectly) {
    // These are cases where it is valid to use document sequences.
    const auto valid = {
        fromjson("{insert: 'coll', documents:[]}"),
        fromjson("{insert: 'coll', documents:[{a:1}]}"),
        fromjson("{insert: 'coll', documents:[{a:1}, {b:1}]}"),
    };

    // These are cases where it is not valid to use document sequences, but the command should still
    // be upconverted to the body.
    const auto invalid = {
        fromjson("{insert: 'coll'}"),
        fromjson("{insert: 'coll', documents:1}"),
        fromjson("{insert: 'coll', documents:[1]}"),
        fromjson("{insert: 'coll', documents:[{a:1}, 1]}"),
        fromjson("{NOT_insert: 'coll', documents:[{a:1}]}"),
    };

    for (const auto& cmd : valid) {
        const auto converted = rpc::upconvertRequest(
            DatabaseName::createDatabaseName_forTest(boost::none, "db"), cmd, 0);
        ASSERT_BSONOBJ_EQ(converted.body, fromjson("{insert: 'coll', $db: 'db'}"));
        ASSERT_EQ(converted.sequences.size(), 1u);
        ASSERT_EQ(converted.sequences[0].name, "documents");

        std::vector<BSONObj> documents;
        for (auto&& elem : cmd["documents"].Obj()) {
            documents.push_back(elem.Obj());
        }

        ASSERT_EQ(converted.sequences[0].objs.size(), documents.size());
        for (size_t i = 0; i < documents.size(); i++) {
            ASSERT_BSONOBJ_EQ(converted.sequences[0].objs[i], documents[i]);

            // The documents should not be copied during upconversion. void* cast to prevent
            // treating pointers as c-strings when printing for failures.
            ASSERT_EQ(static_cast<const void*>(converted.sequences[0].objs[i].sharedBuffer().get()),
                      static_cast<const void*>(cmd.sharedBuffer().get()));
        }
    }

    for (const auto& cmd : invalid) {
        const auto converted = rpc::upconvertRequest(
            DatabaseName::createDatabaseName_forTest(boost::none, "db"), cmd, 0);
        ASSERT_BSONOBJ_EQ(converted.body, addDollarDB(cmd, "db"));
        ASSERT_EQ(converted.sequences.size(), 0u);
    }
}

class InstallIfrContextFromWireTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        _opCtx = cc().makeOperationContext();
    }

    static GenericArguments argsWithIfrFlags() {
        GenericArguments args;
        args.setIfrFlags(std::vector<BSONObj>{});
        return args;
    }

    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(InstallIfrContextFromWireTest, InstallsWireContextWhenIfrFlagsPresent) {
    installIfrContextFromWire(_opCtx.get(), argsWithIfrFlags());

    auto ctx = IncrementalFeatureRolloutContext::tryGet(_opCtx.get());
    ASSERT_TRUE(ctx);
    ASSERT_TRUE(ctx->isInstalledFromWire());
}

TEST_F(InstallIfrContextFromWireTest, NoContextInstalledWhenIfrFlagsAbsent) {
    installIfrContextFromWire(_opCtx.get(), GenericArguments{});

    auto ctx = IncrementalFeatureRolloutContext::tryGet(_opCtx.get());
    ASSERT_FALSE(ctx);
}

TEST_F(InstallIfrContextFromWireTest, NoopInDirectClient) {
    // Nested operations (DBDirectClient sub-commands) share the parent operation's opCtx and must
    // never install, even when their request carries ifrFlags.
    _opCtx->getClient()->setInDirectClient(true);
    installIfrContextFromWire(_opCtx.get(), argsWithIfrFlags());

    ASSERT_FALSE(IncrementalFeatureRolloutContext::tryGet(_opCtx.get()));
}

TEST_F(InstallIfrContextFromWireTest, NestedDirectClientDoesNotMaskOuterWireInstall) {
    // Regression test: a nested operation without ifrFlags (e.g. the 'usersInfo' run by the
    // localhost-auth-bypass check in AuthorizationSession::startRequest()) can run on the
    // parent's opCtx before the parent command is parsed and installs. It must not claim the
    // decoration with a conservative no-flags context, or the parent's wire-provided flag values
    // would be silently discarded by the install-once check.
    _opCtx->getClient()->setInDirectClient(true);
    installIfrContextFromWire(_opCtx.get(), GenericArguments{});
    ASSERT_FALSE(IncrementalFeatureRolloutContext::tryGet(_opCtx.get()));

    _opCtx->getClient()->setInDirectClient(false);
    installIfrContextFromWire(_opCtx.get(), argsWithIfrFlags());

    auto ctx = IncrementalFeatureRolloutContext::tryGet(_opCtx.get());
    ASSERT_TRUE(ctx);
    ASSERT_TRUE(ctx->isInstalledFromWire());
}

TEST_F(InstallIfrContextFromWireTest, SecondInstallIsNoopAndPreservesExistingContext) {
    installIfrContextFromWire(_opCtx.get(), argsWithIfrFlags());
    auto first = IncrementalFeatureRolloutContext::tryGet(_opCtx.get());
    ASSERT_TRUE(first);

    installIfrContextFromWire(_opCtx.get(), GenericArguments{});
    ASSERT_EQ(IncrementalFeatureRolloutContext::tryGet(_opCtx.get()), first);
}

TEST_F(InstallIfrContextFromWireTest, SecondInstallWithIfrFlagsIsNoopAndPreservesExistingContext) {
    // Even when the second call carries ifrFlags, the install-once check prevents overwriting the
    // existing context.
    installIfrContextFromWire(_opCtx.get(), argsWithIfrFlags());
    auto first = IncrementalFeatureRolloutContext::tryGet(_opCtx.get());
    ASSERT_TRUE(first);

    installIfrContextFromWire(_opCtx.get(), argsWithIfrFlags());
    ASSERT_EQ(IncrementalFeatureRolloutContext::tryGet(_opCtx.get()), first);
}

}  // namespace
