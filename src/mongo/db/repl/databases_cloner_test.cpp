/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include <memory>

#include "mongo/client/fetcher.h"
#include "mongo/db/client.h"
#include "mongo/db/json.h"
#include "mongo/db/repl/base_cloner_test_fixture.h"
#include "mongo/db/repl/databases_cloner.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/reporter.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/repl/sync_source_resolver.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/old_thread_pool.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/mongoutils/str.h"

namespace {
using namespace mongo;
using namespace mongo::repl;
using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using LockGuard = stdx::lock_guard<stdx::mutex>;
using UniqueLock = stdx::unique_lock<stdx::mutex>;
using mutex = stdx::mutex;
using NetworkGuard = executor::NetworkInterfaceMock::InNetworkGuard;
using namespace unittest;
using Responses = std::vector<std::pair<std::string, BSONObj>>;

struct CollectionCloneInfo {
    CollectionMockStats stats;
    CollectionBulkLoaderMock* loader = nullptr;
    Status status{ErrorCodes::NotYetInitialized, ""};
};

struct StorageInterfaceResults {
    bool createOplogCalled = false;
    bool insertedOplogEntries = false;
    int oplogEntriesInserted = 0;
    bool droppedUserDBs = false;
    std::vector<std::string> droppedCollections;
    int documentsInsertedCount = 0;
};


class DBsClonerTest : public executor::ThreadPoolExecutorTest {
public:
    DBsClonerTest()
        : _storageInterface{}, _dbWorkThreadPool{OldThreadPool::DoNotStartThreadsTag(), 1} {}

    StorageInterface& getStorage() {
        return _storageInterface;
    }

    OldThreadPool& getDbWorkThreadPool() {
        return _dbWorkThreadPool;
    }

    void scheduleNetworkResponse(std::string cmdName, const BSONObj& obj) {
        NetworkInterfaceMock* net = getNet();
        if (!net->hasReadyRequests()) {
            log() << "The network doesn't have a request to process for this response: " << obj;
        }
        verifyNextRequestCommandName(cmdName);
        scheduleNetworkResponse(net->getNextReadyRequest(), obj);
    }

    void scheduleNetworkResponse(NetworkInterfaceMock::NetworkOperationIterator noi,
                                 const BSONObj& obj) {
        NetworkInterfaceMock* net = getNet();
        Milliseconds millis(0);
        RemoteCommandResponse response(obj, BSONObj(), millis);
        executor::TaskExecutor::ResponseStatus responseStatus(response);
        net->scheduleResponse(noi, net->now(), responseStatus);
    }

    void scheduleNetworkResponse(std::string cmdName, Status errorStatus) {
        NetworkInterfaceMock* net = getNet();
        if (!getNet()->hasReadyRequests()) {
            log() << "The network doesn't have a request to process for the error: " << errorStatus;
        }
        verifyNextRequestCommandName(cmdName);
        net->scheduleResponse(net->getNextReadyRequest(), net->now(), errorStatus);
    }

    void processNetworkResponse(std::string cmdName, const BSONObj& obj) {
        scheduleNetworkResponse(cmdName, obj);
        finishProcessingNetworkResponse();
    }

    void processNetworkResponse(std::string cmdName, Status errorStatus) {
        scheduleNetworkResponse(cmdName, errorStatus);
        finishProcessingNetworkResponse();
    }

    void finishProcessingNetworkResponse() {
        getNet()->runReadyNetworkOperations();
        if (getNet()->hasReadyRequests()) {
            log() << "The network has unexpected requests to process, next req:";
            NetworkInterfaceMock::NetworkOperation req = *getNet()->getNextReadyRequest();
            log() << req.getDiagnosticString();
        }
        ASSERT_FALSE(getNet()->hasReadyRequests());
    }

protected:
    void setUp() override {
        executor::ThreadPoolExecutorTest::setUp();
        launchExecutorThread();

        _storageInterface.createOplogFn = [this](OperationContext* txn,
                                                 const NamespaceString& nss) {
            _storageInterfaceWorkDone.createOplogCalled = true;
            return Status::OK();
        };
        _storageInterface.insertDocumentFn =
            [this](OperationContext* txn, const NamespaceString& nss, const BSONObj& doc) {
                ++_storageInterfaceWorkDone.documentsInsertedCount;
                return Status::OK();
            };
        _storageInterface.insertDocumentsFn = [this](
            OperationContext* txn, const NamespaceString& nss, const std::vector<BSONObj>& ops) {
            _storageInterfaceWorkDone.insertedOplogEntries = true;
            ++_storageInterfaceWorkDone.oplogEntriesInserted;
            return Status::OK();
        };
        _storageInterface.dropCollFn = [this](OperationContext* txn, const NamespaceString& nss) {
            _storageInterfaceWorkDone.droppedCollections.push_back(nss.ns());
            return Status::OK();
        };
        _storageInterface.dropUserDBsFn = [this](OperationContext* txn) {
            _storageInterfaceWorkDone.droppedUserDBs = true;
            return Status::OK();
        };
        _storageInterface.createCollectionForBulkFn =
            [this](const NamespaceString& nss,
                   const CollectionOptions& options,
                   const BSONObj idIndexSpec,
                   const std::vector<BSONObj>& secondaryIndexSpecs) {
                // Get collection info from map.
                const auto collInfo = &_collections[nss];
                if (collInfo->stats.initCalled) {
                    log() << "reusing collection during test which may cause problems, ns:" << nss;
                }
                (collInfo->loader = new CollectionBulkLoaderMock(&collInfo->stats))
                    ->init(nullptr, nullptr, secondaryIndexSpecs);

                return StatusWith<std::unique_ptr<CollectionBulkLoader>>(
                    std::unique_ptr<CollectionBulkLoader>(collInfo->loader));
            };

        _dbWorkThreadPool.startThreads();
    }

    void tearDown() override {
        executor::ThreadPoolExecutorTest::shutdownExecutorThread();
        executor::ThreadPoolExecutorTest::joinExecutorThread();

        _dbWorkThreadPool.join();

        executor::ThreadPoolExecutorTest::tearDown();
    }

    /**
     * Note: An empty cmdName will skip validation.
     */
    void verifyNextRequestCommandName(std::string cmdName) {
        const auto net = getNet();
        ASSERT_TRUE(net->hasReadyRequests());

        if (cmdName != "") {
            const NetworkInterfaceMock::NetworkOperationIterator req =
                net->getFrontOfUnscheduledQueue();
            const BSONObj reqBSON = req->getRequest().cmdObj;
            const BSONElement cmdElem = reqBSON.firstElement();
            auto reqCmdName = cmdElem.fieldNameStringData();
            ASSERT_EQ(cmdName, reqCmdName);
        }
    }

    Status playResponses(Responses responses, bool isLastBatchOfResponses) {
        NetworkInterfaceMock* net = getNet();
        int processedRequests(0);
        const int expectedResponses(responses.size());

        Date_t lastLog{Date_t::now()};
        while (true) {
            NetworkGuard guard(net);
            if (!net->hasReadyRequests() && processedRequests < expectedResponses) {
                guard.dismiss();
                sleepmicros(10);
                continue;
            }

            auto noi = net->getNextReadyRequest();
            const BSONObj reqBSON = noi->getRequest().cmdObj;
            const BSONElement cmdElem = reqBSON.firstElement();
            auto cmdName = cmdElem.fieldNameStringData();
            auto expectedName = responses[processedRequests].first;
            if (responses[processedRequests].first != "" &&
                !cmdName.equalCaseInsensitive(expectedName)) {
                // Error, wrong response for request name
                log() << "ERROR: expected " << expectedName
                      << " but the request was: " << noi->getRequest().cmdObj;
            }

            // process fixed set of responses
            log() << "Sending response for network request:";
            log() << "     req: " << noi->getRequest().dbname << "." << noi->getRequest().cmdObj;
            log() << "     resp:" << responses[processedRequests].second;
            net->scheduleResponse(
                noi,
                net->now(),
                ResponseStatus(RemoteCommandResponse(
                    responses[processedRequests].second, BSONObj(), Milliseconds(10))));

            if ((Date_t::now() - lastLog) > Seconds(1)) {
                lastLog = Date_t();
                log() << net->getDiagnosticString();
                net->logQueues();
            }
            net->runReadyNetworkOperations();

            guard.dismiss();
            if (++processedRequests >= expectedResponses) {
                log() << "done processing expected requests ";
                break;  // once we have processed all requests, continue;
            }
        }

        if (!isLastBatchOfResponses) {
            return Status::OK();
        }

        NetworkGuard guard(net);
        if (net->hasReadyRequests()) {
            // Error.
            log() << "There are unexpected requests left:";
            while (net->hasReadyRequests()) {
                auto noi = net->getNextReadyRequest();
                log() << "cmd: " << noi->getRequest().cmdObj.toString();
            }
            return {ErrorCodes::CommandFailed, "There were unprocessed requests."};
        }

        return Status::OK();
    };

    void runCompleteClone(Responses responses) {
        Status result{Status::OK()};
        bool done = false;
        stdx::mutex mutex;
        stdx::condition_variable cvDone;
        DatabasesCloner cloner{&getStorage(),
                               &getExecutor(),
                               &getDbWorkThreadPool(),
                               HostAndPort{"local:1234"},
                               [](const BSONObj&) { return true; },
                               [&](const Status& status) {
                                   UniqueLock lk(mutex);
                                   log() << "setting result to " << status;
                                   done = true;
                                   result = status;
                                   cvDone.notify_all();
                               }};

        ASSERT_OK(cloner.startup());
        ASSERT_TRUE(cloner.isActive());

        ASSERT_OK(playResponses(responses, true));
        UniqueLock lk(mutex);
        // If the cloner is active, wait for cond_var to be signaled when it completes.
        if (!done) {
            cvDone.wait(lk);
        }
        ASSERT_FALSE(cloner.isActive());
        ASSERT_OK(result);
    };

private:
    StorageInterfaceMock _storageInterface;
    OldThreadPool _dbWorkThreadPool;
    std::map<NamespaceString, CollectionMockStats> _collectionStats;
    std::map<NamespaceString, CollectionCloneInfo> _collections;
    StorageInterfaceResults _storageInterfaceWorkDone;
};

// TODO: Move tests here from data_replicator_test here and figure out
//       how to script common data (dbs, collections, indexes) scenarios w/failures.

TEST_F(DBsClonerTest, InvalidConstruction) {
    HostAndPort source{"local:1234"};
    auto includeDbPred = [](const BSONObj&) { return true; };
    auto finishFn = [](const Status&) {};

    // Null storage interface.
    ASSERT_THROWS_CODE_AND_WHAT(
        DatabasesCloner(
            nullptr, &getExecutor(), &getDbWorkThreadPool(), source, includeDbPred, finishFn),
        UserException,
        ErrorCodes::InvalidOptions,
        "storage interface must be provided.");

    // Null task executor.
    ASSERT_THROWS_CODE_AND_WHAT(
        DatabasesCloner(
            &getStorage(), nullptr, &getDbWorkThreadPool(), source, includeDbPred, finishFn),
        UserException,
        ErrorCodes::InvalidOptions,
        "executor must be provided.");

    // Null db worker thread pool.
    ASSERT_THROWS_CODE_AND_WHAT(
        DatabasesCloner(&getStorage(), &getExecutor(), nullptr, source, includeDbPred, finishFn),
        UserException,
        ErrorCodes::InvalidOptions,
        "db worker thread pool must be provided.");

    // Empty source.
    ASSERT_THROWS_CODE_AND_WHAT(
        DatabasesCloner(
            &getStorage(), &getExecutor(), &getDbWorkThreadPool(), {}, includeDbPred, finishFn),
        UserException,
        ErrorCodes::InvalidOptions,
        "source must be provided.");

    // Null include database predicate.
    ASSERT_THROWS_CODE_AND_WHAT(
        DatabasesCloner(
            &getStorage(), &getExecutor(), &getDbWorkThreadPool(), source, {}, finishFn),
        UserException,
        ErrorCodes::InvalidOptions,
        "includeDbPred must be provided.");

    // Null finish callback.
    ASSERT_THROWS_CODE_AND_WHAT(
        DatabasesCloner(
            &getStorage(), &getExecutor(), &getDbWorkThreadPool(), source, includeDbPred, {}),
        UserException,
        ErrorCodes::InvalidOptions,
        "finishFn must be provided.");
}

TEST_F(DBsClonerTest, FailsOnListDatabases) {
    Status result{Status::OK()};
    Status expectedResult{ErrorCodes::BadValue, "foo"};
    DatabasesCloner cloner{&getStorage(),
                           &getExecutor(),
                           &getDbWorkThreadPool(),
                           HostAndPort{"local:1234"},
                           [](const BSONObj&) { return true; },
                           [&result](const Status& status) {
                               log() << "setting result to " << status;
                               result = status;
                           }};

    ASSERT_OK(cloner.startup());
    ASSERT_TRUE(cloner.isActive());

    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);
    processNetworkResponse("listDatabases", expectedResult);
    ASSERT_EQ(result, expectedResult);
}

TEST_F(DBsClonerTest, FailsOnListCollectionsOnOnlyDatabase) {
    Status result{Status::OK()};
    DatabasesCloner cloner{&getStorage(),
                           &getExecutor(),
                           &getDbWorkThreadPool(),
                           HostAndPort{"local:1234"},
                           [](const BSONObj&) { return true; },
                           [&result](const Status& status) {
                               log() << "setting result to " << status;
                               result = status;
                           }};

    ASSERT_OK(cloner.startup());
    ASSERT_TRUE(cloner.isActive());

    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);
    scheduleNetworkResponse("listDatabases",
                            fromjson("{ok:1, databases:[{name:'a'}]}"));  // listDatabases
    net->runReadyNetworkOperations();
    ASSERT_TRUE(cloner.isActive());
    processNetworkResponse("listCollections",
                           Status{ErrorCodes::NoSuchKey, "fake"});  // listCollections

    cloner.join();
    ASSERT_FALSE(cloner.isActive());
    ASSERT_NOT_OK(result);
}
TEST_F(DBsClonerTest, FailsOnListCollectionsOnFirstOfTwoDatabases) {
    Status result{Status::OK()};
    Status expectedStatus{ErrorCodes::NoSuchKey, "fake"};
    DatabasesCloner cloner{&getStorage(),
                           &getExecutor(),
                           &getDbWorkThreadPool(),
                           HostAndPort{"local:1234"},
                           [](const BSONObj&) { return true; },
                           [&result](const Status& status) {
                               log() << "setting result to " << status;
                               result = status;
                           }};

    ASSERT_OK(cloner.startup());
    ASSERT_TRUE(cloner.isActive());

    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);
    // listDatabases
    scheduleNetworkResponse("listDatabases",
                            fromjson("{ok:1, databases:[{name:'a'}, {name:'b'}]}"));
    net->runReadyNetworkOperations();
    ASSERT_TRUE(cloner.isActive());
    // listCollections (db:a)
    scheduleNetworkResponse("listCollections", expectedStatus);
    // listCollections (db:b)
    processNetworkResponse("listCollections",
                           fromjson("{ok:1, cursor:{id:NumberLong(0), "
                                    "ns:'b.$cmd.listCollections', "
                                    "firstBatch:[]}}"));

    cloner.join();
    ASSERT_FALSE(cloner.isActive());
    ASSERT_EQ(result, expectedStatus);
}

TEST_F(DBsClonerTest, SingleDatabaseCopiesCompletely) {
    const Responses resps = {
        // Clone Start
        // listDatabases
        {"listDatabases", fromjson("{ok:1, databases:[{name:'a'}]}")},
        // listCollections for "a"
        {"listCollections",
         fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listCollections', firstBatch:["
                  "{name:'a', options:{}} "
                  "]}}")},
        // listIndexes:a
        {
            "listIndexes",
            fromjson(str::stream()
                     << "{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listIndexes.a', firstBatch:["
                        "{v:"
                     << OplogEntry::kOplogVersion
                     << ", key:{_id:1}, name:'_id_', ns:'a.a'}]}}")},
        // find:a
        {"find",
         fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.a', firstBatch:["
                  "{_id:1, a:1} "
                  "]}}")},
        // Clone Done
    };
    runCompleteClone(resps);
}

TEST_F(DBsClonerTest, TwoDatabasesCopiesCompletely) {
    const Responses resps =
        {
            // Clone Start
            // listDatabases
            {"listDatabases", fromjson("{ok:1, databases:[{name:'a'}, {name:'b'}]}")},
            // listCollections for "a"
            {"listCollections",
             fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listCollections', firstBatch:["
                      "{name:'a', options:{}} "
                      "]}}")},
            // listCollections for "b"
            {"listCollections",
             fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'b.$cmd.listCollections', firstBatch:["
                      "{name:'b', options:{}} "
                      "]}}")},
            // listIndexes:a
            {"listIndexes",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(0), ns:'a.$cmd.listIndexes.a', firstBatch:["
                         "{v:"
                      << OplogEntry::kOplogVersion
                      << ", key:{_id:1}, name:'_id_', ns:'a.a'}]}}")},
            // listIndexes:b
            {"listIndexes",
             fromjson(str::stream()
                      << "{ok:1, cursor:{id:NumberLong(0), ns:'b.$cmd.listIndexes.b', firstBatch:["
                         "{v:"
                      << OplogEntry::kOplogVersion
                      << ", key:{_id:1}, name:'_id_', ns:'b.b'}]}}")},
            // find:a
            {"find",
             fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'a.a', firstBatch:["
                      "{_id:1, a:1} "
                      "]}}")},
            // find:b
            {"find",
             fromjson("{ok:1, cursor:{id:NumberLong(0), ns:'b.b', firstBatch:["
                      "{_id:2, a:1},{_id:3, b:1}"
                      "]}}")},
        };
    runCompleteClone(resps);
}

}  // namespace
