/**
 *    Copyright 2017 MongoDB Inc.
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

#include "mongo/db/repl/abstract_oplog_fetcher_test_fixture.h"

#include "mongo/db/repl/oplog_entry.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {

namespace {

/**
 * Creates an OplogEntry using given field values.
 */
repl::OplogEntry makeOplogEntry(repl::OpTime opTime,
                                long long hash,
                                repl::OpTypeEnum opType,
                                NamespaceString nss,
                                BSONObj object) {
    return repl::OplogEntry(opTime,                           // optime
                            hash,                             // hash
                            opType,                           // opType
                            nss,                              // namespace
                            boost::none,                      // uuid
                            boost::none,                      // fromMigrate
                            repl::OplogEntry::kOplogVersion,  // version
                            object,                           // o
                            boost::none,                      // o2
                            {},                               // sessionInfo
                            boost::none,                      // wall clock time
                            boost::none,                      // statement id
                            boost::none,   // optime of previous write within same transaction
                            boost::none,   // pre-image optime
                            boost::none);  // post-image optime
}

}  // namespace

ShutdownState::ShutdownState() = default;

Status ShutdownState::getStatus() const {
    return _status;
}

void ShutdownState::operator()(const Status& status) {
    _status = status;
}

BSONObj AbstractOplogFetcherTest::makeNoopOplogEntry(OpTimeWithHash opTimeWithHash) {
    return makeOplogEntry(opTimeWithHash.opTime,
                          opTimeWithHash.value,
                          OpTypeEnum::kNoop,
                          NamespaceString("test.t"),
                          BSONObj())
        .toBSON();
}

BSONObj AbstractOplogFetcherTest::makeNoopOplogEntry(OpTime opTime, long long hash) {
    return makeNoopOplogEntry({hash, opTime});
}

BSONObj AbstractOplogFetcherTest::makeNoopOplogEntry(Seconds seconds, long long hash) {
    return makeNoopOplogEntry({{seconds, 0}, 1LL}, hash);
}

BSONObj AbstractOplogFetcherTest::makeCursorResponse(CursorId cursorId,
                                                     Fetcher::Documents oplogEntries,
                                                     bool isFirstBatch,
                                                     const NamespaceString& nss) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder cursorBob(bob.subobjStart("cursor"));
        cursorBob.append("id", cursorId);
        cursorBob.append("ns", nss.toString());
        {
            BSONArrayBuilder batchBob(
                cursorBob.subarrayStart(isFirstBatch ? "firstBatch" : "nextBatch"));
            for (auto oplogEntry : oplogEntries) {
                batchBob.append(oplogEntry);
            }
        }
    }
    bob.append("ok", 1);
    return bob.obj();
}

void AbstractOplogFetcherTest::setUp() {
    executor::ThreadPoolExecutorTest::setUp();
    launchExecutorThread();

    lastFetched = {456LL, {{123, 0}, 1}};
}

executor::RemoteCommandRequest AbstractOplogFetcherTest::processNetworkResponse(
    executor::RemoteCommandResponse response, bool expectReadyRequestsAfterProcessing) {

    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);
    unittest::log() << "scheduling response.";
    auto request = net->scheduleSuccessfulResponse(response);
    unittest::log() << "running network ops.";
    net->runReadyNetworkOperations();
    unittest::log() << "checking for more requests";
    ASSERT_EQUALS(expectReadyRequestsAfterProcessing, net->hasReadyRequests());
    unittest::log() << "returning consumed request";
    return request;
}

executor::RemoteCommandRequest AbstractOplogFetcherTest::processNetworkResponse(
    BSONObj obj, bool expectReadyRequestsAfterProcessing) {
    return processNetworkResponse({obj, rpc::makeEmptyMetadata(), Milliseconds(0)},
                                  expectReadyRequestsAfterProcessing);
}

}  // namespace repl
}  // namespace mango
