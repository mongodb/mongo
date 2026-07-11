// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/db/database_name.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

const auto sleepCmd = fromjson(R"({sleep: 1, locks: 'none', secs: 100})");
constexpr std::string_view kAppName = "DBClientConnectionTest"sv;

class DBClientConnectionFixture : public unittest::Test {
public:
    static std::unique_ptr<DBClientConnection> makeConn(std::string_view name = kAppName) {
        auto swConn = unittest::getFixtureConnectionString().connect(name);
        uassertStatusOK(swConn.getStatus());

        auto conn = dynamic_cast<DBClientConnection*>(swConn.getValue().release());
        return std::unique_ptr<DBClientConnection>{conn};
    }

    void tearDown() override {
        // resmoke hangs if there are any ops still running on the server, so try to clean up after
        // ourselves.
        auto conn = makeConn(std::string{kAppName} + "-cleanup");

        BSONObj currOp;
        if (!conn->runCommand(DatabaseName::kAdmin, BSON("currentOp" << 1), currOp))
            uassertStatusOK(getStatusFromCommandResult(currOp));

        for (auto&& op : currOp["inprog"].Obj()) {
            try {
                if (op["clientMetadata"]["application"]["name"].String() != kAppName) {
                    continue;
                }
            } catch (const DBException&) {
                // Ignore ops that don't have the right format since they can't be the one we are
                // looking for.
                continue;
            }

            // Ignore failures to clean up.
            BSONObj ignored;
            (void)conn->runCommand(
                DatabaseName::kAdmin, BSON("killOp" << 1 << "op" << op["opid"]), ignored);
        }
    }
};

TEST_F(DBClientConnectionFixture, shutdownWorksIfCalledFirst) {
    auto conn = makeConn();

    conn->shutdownAndDisallowReconnect();

    BSONObj reply;
    ASSERT_THROWS(conn->runCommand(DatabaseName::kAdmin, sleepCmd, reply),
                  ExceptionFor<ErrorCategory::NetworkError>);  // Currently SocketException.
}

TEST_F(DBClientConnectionFixture, shutdownWorksIfRunCommandInProgress) {
    auto conn = makeConn();

    stdx::thread shutdownThread([&] {
        // Try to make this run while the connection is blocked in recv.
        sleepmillis(100);
        conn->shutdownAndDisallowReconnect();
    });
    ON_BLOCK_EXIT([&] { shutdownThread.join(); });

    BSONObj reply;
    ASSERT_THROWS(conn->runCommand(DatabaseName::kAdmin, sleepCmd, reply),
                  ExceptionFor<ErrorCategory::NetworkError>);  // Currently HostUnreachable.
}

}  // namespace
}  // namespace mongo
