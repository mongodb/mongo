/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <boost/scoped_ptr.hpp>
#include <boost/thread.hpp>

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/election_winner_declarer.h"
#include "mongo/db/repl/network_interface_mock.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/stdx/functional.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

using boost::scoped_ptr;

namespace mongo {
namespace repl {
namespace {

    using unittest::assertGet;

    using RemoteCommandRequest = ReplicationExecutor::RemoteCommandRequest;

    bool stringContains(const std::string &haystack, const std::string& needle) {
        return haystack.find(needle) != std::string::npos;
    }


    class ElectionWinnerDeclarerTest : public mongo::unittest::Test {
    public:
        virtual void setUp() {
            std::string setName = "rs0";
            long long winnerId = 0;
            long long term = 1;
            std::vector<HostAndPort> hosts = {HostAndPort("host0"),
                                              HostAndPort("host1"),
                                              HostAndPort("host2")};

            _declarer.reset(new ElectionWinnerDeclarer::Algorithm(setName,
                                                                  winnerId,
                                                                  term,
                                                                  hosts));
        }

        virtual void tearDown() {
            _declarer.reset(NULL);
        }

    protected:
        int64_t countLogLinesContaining(const std::string& needle) {
            return std::count_if(getCapturedLogMessages().begin(),
                                 getCapturedLogMessages().end(),
                                 stdx::bind(stringContains,
                                            stdx::placeholders::_1,
                                            needle));
        }

        bool hasReceivedSufficientResponses() {
            return _declarer->hasReceivedSufficientResponses();
        }

        Status getStatus() {
            return _declarer->getStatus();
        }

        void processResponse(const RemoteCommandRequest& request, const ResponseStatus& response) {
            _declarer->processResponse(request, response);
        }

        RemoteCommandRequest requestFrom(std::string hostname) {
            return RemoteCommandRequest(HostAndPort(hostname),
                                        "", // fields do not matter in ElectionWinnerDeclarer
                                        BSONObj(),
                                        Milliseconds(0));
        }

        ResponseStatus badResponseStatus() {
            return ResponseStatus(ErrorCodes::NodeNotFound, "not on my watch");
        }

        ResponseStatus staleTermResponse() {
            return ResponseStatus(NetworkInterfaceMock::Response(BSON("ok" << 0
                                                                   << "code" << ErrorCodes::BadValue
                                                                   << "errmsg"
                                                                   << "term has already passed"
                                                                   << "term" << 3),
                                                                 Milliseconds(10)));
        }

        ResponseStatus alreadyAnotherPrimaryResponse() {
            return ResponseStatus(NetworkInterfaceMock::Response(BSON("ok" << 0
                                                                   << "code" << ErrorCodes::BadValue
                                                                   << "errmsg"
                                                                   << "term already has a primary"
                                                                   << "term" << 1),
                                                                 Milliseconds(10)));
        }

        ResponseStatus differentConfigVersionResponse() {
            return ResponseStatus(NetworkInterfaceMock::Response(BSON("ok" << 0
                                                                  << "code" << ErrorCodes::BadValue
                                                                  << "errmsg"
                                                                  << "config version does not match"
                                                                  << "term" << 1),
                                                                 Milliseconds(10)));
        }

        ResponseStatus differentSetNameResponse() {
            return ResponseStatus(NetworkInterfaceMock::Response(BSON("ok" << 0
                                                                  << "code" << ErrorCodes::BadValue
                                                                  << "errmsg"
                                                                  << "replSet name does not match"
                                                                  << "term" << 1),
                                                                 Milliseconds(10)));
        }

        ResponseStatus goodResponse() {
            return ResponseStatus(NetworkInterfaceMock::Response(BSON("ok" << 1
                                                                  << "term" << 1),
                                                                 Milliseconds(10)));
        }

    private:
        scoped_ptr<ElectionWinnerDeclarer::Algorithm> _declarer;

    };

    TEST_F(ElectionWinnerDeclarerTest, FinishWithOnlyGoodResponses) {
        ASSERT_FALSE(hasReceivedSufficientResponses());
        processResponse(requestFrom("host0"), goodResponse());
        ASSERT_FALSE(hasReceivedSufficientResponses());
        processResponse(requestFrom("host1"), goodResponse());
        ASSERT_FALSE(hasReceivedSufficientResponses());
        processResponse(requestFrom("host2"), goodResponse());
        ASSERT_TRUE(hasReceivedSufficientResponses());
        ASSERT_OK(getStatus());
    }

    TEST_F(ElectionWinnerDeclarerTest, FailedDueToStaleTerm) {
        startCapturingLogMessages();
        ASSERT_FALSE(hasReceivedSufficientResponses());
        processResponse(requestFrom("host0"), goodResponse());
        ASSERT_FALSE(hasReceivedSufficientResponses());
        processResponse(requestFrom("host1"), staleTermResponse());
        ASSERT_TRUE(hasReceivedSufficientResponses());
        ASSERT_EQUALS(1, countLogLinesContaining("Got error response from host1"));
        stopCapturingLogMessages();
        ASSERT_EQUALS(getStatus().reason(), "term has already passed");
    }

    TEST_F(ElectionWinnerDeclarerTest, FailedDueToAnotherPrimary) {
        startCapturingLogMessages();
        ASSERT_FALSE(hasReceivedSufficientResponses());
        processResponse(requestFrom("host0"), goodResponse());
        ASSERT_FALSE(hasReceivedSufficientResponses());
        processResponse(requestFrom("host1"), alreadyAnotherPrimaryResponse());
        ASSERT_TRUE(hasReceivedSufficientResponses());
        ASSERT_EQUALS(1, countLogLinesContaining("Got error response from host1"));
        stopCapturingLogMessages();
        ASSERT_EQUALS(getStatus().reason(), "term already has a primary");
    }

    TEST_F(ElectionWinnerDeclarerTest, FailedDueToDifferentSetName) {
        startCapturingLogMessages();
        ASSERT_FALSE(hasReceivedSufficientResponses());
        processResponse(requestFrom("host0"), goodResponse());
        ASSERT_FALSE(hasReceivedSufficientResponses());
        processResponse(requestFrom("host1"), differentSetNameResponse());
        ASSERT_TRUE(hasReceivedSufficientResponses());
        ASSERT_EQUALS(1, countLogLinesContaining("Got error response from host1"));
        stopCapturingLogMessages();
        ASSERT_EQUALS(getStatus().reason(), "replSet name does not match");
    }

    TEST_F(ElectionWinnerDeclarerTest, FinishWithOnlyGoodResponsesAndMissingNode) {
        startCapturingLogMessages();
        ASSERT_FALSE(hasReceivedSufficientResponses());
        processResponse(requestFrom("host0"), goodResponse());
        ASSERT_FALSE(hasReceivedSufficientResponses());
        processResponse(requestFrom("host1"), badResponseStatus());
        ASSERT_FALSE(hasReceivedSufficientResponses());
        processResponse(requestFrom("host2"), goodResponse());
        ASSERT_TRUE(hasReceivedSufficientResponses());
        ASSERT_EQUALS(1, countLogLinesContaining("Got failed response from host1"));
        stopCapturingLogMessages();
        ASSERT_OK(getStatus());
    }

    TEST_F(ElectionWinnerDeclarerTest, FinishWithOnlyMissingResponses) {
        startCapturingLogMessages();
        ASSERT_FALSE(hasReceivedSufficientResponses());
        processResponse(requestFrom("host0"), badResponseStatus());
        ASSERT_FALSE(hasReceivedSufficientResponses());
        processResponse(requestFrom("host1"), badResponseStatus());
        ASSERT_FALSE(hasReceivedSufficientResponses());
        processResponse(requestFrom("host2"), badResponseStatus());
        ASSERT_TRUE(hasReceivedSufficientResponses());
        ASSERT_EQUALS(1, countLogLinesContaining("Got failed response from host0"));
        ASSERT_EQUALS(1, countLogLinesContaining("Got failed response from host1"));
        ASSERT_EQUALS(1, countLogLinesContaining("Got failed response from host2"));
        stopCapturingLogMessages();
        ASSERT_OK(getStatus());
    }

}  // namespace
}  // namespace repl
}  // namespace mongo
