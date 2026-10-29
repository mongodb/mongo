// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/rewrite_state_change_errors.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::rpc {
namespace {

class RewriteStateChangeErrorsTest : public unittest::Test {
public:
    RewriteStateChangeErrorsTest() {
        sc = ServiceContext::make();
        cc = sc->getService()->makeClient("test", nullptr);
        opCtx = sc->makeOperationContext(cc.get());
    }

    void setUp() override {
        // Whole feature only happens on mongos.
        serverGlobalParams.clusterRole = ClusterRole::RouterServer;
        RewriteStateChangeErrors::setEnabled(&*sc, true);
    }

    void tearDown() override {
        serverGlobalParams.clusterRole = ClusterRole::None;
    }

    /** Run rewrite on `obj` and return what it was remapped to if anything. */
    BSONObj rewriteObj(const BSONObj& obj) {
        if (auto newDoc = RewriteStateChangeErrors::rewrite(obj, &*opCtx))
            return *newDoc;
        return obj;
    }

    /** Make an error node corresponding to `ec`. */
    BSONObj errorObject(ErrorCodes::Error ec) {
        BSONObjBuilder bob;
        if (ec == ErrorCodes::OK)
            bob.append("ok", 1.);
        else
            bob.append("ok", 0.)
                .append("code", static_cast<int>(ec))
                .append("codeName", ErrorCodes::errorString(ec));
        return bob.obj();
    }

    /** Make an error node corresponding to `ec` with `errmsg`. */
    BSONObj errorObject(ErrorCodes::Error ec, std::string errmsg) {
        return BSONObjBuilder(errorObject(ec)).append("errmsg", errmsg).obj();
    }

    /** A few codes and what we expect them to be rewritten to. */
    struct InOutCode {
        ErrorCodes::Error in;
        ErrorCodes::Error out;
    };
    static constexpr InOutCode errorCodeScenarios[] = {
        {ErrorCodes::InterruptedAtShutdown, ErrorCodes::HostUnreachable},
        {ErrorCodes::ShutdownInProgress, ErrorCodes::HostUnreachable},
        {ErrorCodes::OK, ErrorCodes::OK},
        {ErrorCodes::BadValue, ErrorCodes::BadValue},
    };

    ServiceContext::UniqueServiceContext sc;
    ServiceContext::UniqueClient cc;
    ServiceContext::UniqueOperationContext opCtx;
};

// Rewrite Shutdown errors received from proxied commands.
TEST_F(RewriteStateChangeErrorsTest, Enabled) {
    for (auto&& [in, out] : errorCodeScenarios) {
        ASSERT_BSONOBJ_EQ(rewriteObj(errorObject(in)), errorObject(out));
    }
}

// Check that rewrite behavior can be disabled per-ServiceContext.
TEST_F(RewriteStateChangeErrorsTest, Disabled) {
    RewriteStateChangeErrors::setEnabled(&*sc, false);
    ASSERT_BSONOBJ_EQ(rewriteObj(errorObject(ErrorCodes::InterruptedAtShutdown)),
                      errorObject(ErrorCodes::InterruptedAtShutdown));
}

// Check that rewrite behavior can be disabled per-opCtx.
TEST_F(RewriteStateChangeErrorsTest, DisabledOpCtx) {
    RewriteStateChangeErrors::setEnabled(&*opCtx, false);
    ASSERT_BSONOBJ_EQ(rewriteObj(errorObject(ErrorCodes::InterruptedAtShutdown)),
                      errorObject(ErrorCodes::InterruptedAtShutdown));
}

// If locally shutting down, then shutdown errors must not be rewritten.
TEST_F(RewriteStateChangeErrorsTest, LocalShutdown) {
    sc->setKillAllOperations();
    ASSERT_BSONOBJ_EQ(rewriteObj(errorObject(ErrorCodes::InterruptedAtShutdown)),
                      errorObject(ErrorCodes::InterruptedAtShutdown));
}

TEST_F(RewriteStateChangeErrorsTest, RewriteErrmsg) {
    const std::pair<std::string, std::string> scenarios[] = {
        {"not master", "(NOT_PRIMARY)"},
        {"node is recovering", "(NODE_IS_RECOVERING)"},
        {"NOT master", "NOT master"},
        {"", ""},
        {" not masternot master ", " (NOT_PRIMARY)(NOT_PRIMARY) "},
        {"not masternode is recovering", "(NOT_PRIMARY)(NODE_IS_RECOVERING)"},
    };
    for (auto&& io : scenarios) {
        ASSERT_BSONOBJ_EQ(rewriteObj(errorObject(ErrorCodes::InterruptedAtShutdown, io.first)),
                          errorObject(ErrorCodes::HostUnreachable, io.second));
    }
}

// Can find and rewrite the `writeConcernError` in an `ok:1` response.
TEST_F(RewriteStateChangeErrorsTest, WriteConcernError) {
    // Make an OK object, and append a `writeConcernError` subobject bearing the `ec` error.
    auto wceObject = [&](ErrorCodes::Error ec) {
        return BSONObjBuilder(errorObject(ErrorCodes::OK))
            .append("writeConcernError", errorObject(ec))
            .obj();
    };
    for (auto&& [in, out] : errorCodeScenarios) {
        ASSERT_BSONOBJ_EQ(rewriteObj(wceObject(in)), wceObject(out));
    }
}

// Can find and rewrite the `writeErrors` array elements in an `ok:1` response.
TEST_F(RewriteStateChangeErrorsTest, WriteErrors) {
    // Make an OK object, and append a `writeErrors` subobject bearing the `ec` errors.
    auto weObject = [&](std::vector<ErrorCodes::Error> ecVec) {
        BSONObjBuilder bob(errorObject(ErrorCodes::OK));
        {
            BSONArrayBuilder bab(bob.subarrayStart("writeErrors"));
            for (ErrorCodes::Error ec : ecVec)
                bab.append(errorObject(ec));
        }
        return bob.obj();
    };
    for (auto&& [in, out] : errorCodeScenarios) {
        ASSERT_BSONOBJ_EQ(rewriteObj(weObject({in})), weObject({out}));
    }
    // Now try all the errorCodeScenarios as a single array of `writeErrors`.
    std::vector<ErrorCodes::Error> allIn, allOut;
    for (auto&& [in, out] : errorCodeScenarios) {
        allIn.push_back(in);
        allOut.push_back(out);
    }
    ASSERT_BSONOBJ_EQ(rewriteObj(weObject(allIn)), weObject(allOut));
}

}  // namespace
}  // namespace mongo::rpc
