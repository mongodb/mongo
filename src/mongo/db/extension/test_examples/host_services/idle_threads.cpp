/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

#include <thread>

/**
 * $idleThreads tests the idle-thread-marking functionality by spawning a thread, marking it as idle
 * and unmarking it later, which will affect the output of mongodb-bt-if-active in gdb.
 */
namespace sdk = mongo::extension::sdk;
using namespace mongo;

class IdleThreadsAstNode
    : public sdk::TestAstNode<sdk::shared_test_stages::TransformLogicalAggStage> {
public:
    IdleThreadsAstNode(std::string_view stageName, BSONObj arguments)
        : sdk::TestAstNode<sdk::shared_test_stages::TransformLogicalAggStage>(stageName,
                                                                              arguments) {}
    /**
     * This extension will demonstrate the functionality of marking threads as idle. Given that
     * this feature is only reflected in gdb, there is no unit or e2e test. Rather, to test the
     * flow, you can recreate the following steps:
     *
     * 1. Build 'install-dist-test' with the 'debug' profile:
     *      bazel build --config=dbg install-dist-test
     * 2. Run gdb with the correct parameters to enable the extension:
     *      /opt/mongodbtoolchain/v5/bin/gdb --args bazel-bin/install/bin/mongod \
     *          --setParameter featureFlagExtensionsAPI=true --loadExtensions idle_threads
     * 3. Once in gdb, set a couple of breakpoints for each of the 'sleep(1)' calls:
     *      (gdb) b src/mongo/db/extension/test_examples/host_services/idle_threads.cpp:<line>
     * 4. Run the server:
     *      (gdb) r
     * 5. In another terminal, spin up a mongo shell:
     *      bazel-bin/install/bin/mongo
     * 6. Once in the mongo shell, run an explain command with the $idleThreads stage:
     *      db.test.explain().aggregate([{$idleThreads: {}}])
     * 7. Back in gdb, when hitting the breakpoints, inspect the spawned thread:
     *      (gdb) t <thread_id>
     * 8. Run the command for a multi-threaded stacktrace:
     *      (gdb) mongodb-bt-if-active
     * 9. On the first breakpoint the thread isn't idle yet, so it will print a stacktrace.
     * 10. Continue execution until the next breakpoint:
     *      (gdb) c
     * 11. Now on the second breakpoint repeat step 8, and since thread is marked as idle this
     *     message is printed:
     *      "Thread is idle at <file_path>/idle_threads.cpp:<line_number>"
     * 12. Continue execution until the next breakpoint:
     *      (gdb) c
     * 13. On the third and final breakpoint, since the idleThreadHandle has gone out of scope, the
     *     thread is no longer idle. Repeating step 8 will print a stacktrace again.
     */

    static void threadFunction() {
        sleep(1);
        do {
            auto idleThreadHandle = sdk::HostServicesHandle::getHostServices()->markIdleThread(
                MONGO_EXTENSION_IDLE_LOCATION);
            sleep(1);
        } while (false);
        sleep(1);
    }

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        std::thread idleThread(threadFunction);
        idleThread.join();

        return sdk::TestAstNode<sdk::shared_test_stages::TransformLogicalAggStage>::bind();
    }
};

DEFAULT_PARSE_NODE(IdleThreads);

using IdleThreadsStageDescriptor = sdk::TestStageDescriptor<"$idleThreads",
                                                            IdleThreadsParseNode,
                                                            true /* ExpectEmptyStageDefinition */>;

DEFAULT_EXTENSION(IdleThreads)
REGISTER_EXTENSION(IdleThreadsExtension)
DEFINE_GET_EXTENSION()
