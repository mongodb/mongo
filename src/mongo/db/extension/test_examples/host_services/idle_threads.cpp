// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

#include <string_view>
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
            auto idleThreadHandle =
                sdk::HostServicesAPI::getInstance()->markIdleThread(MONGO_EXTENSION_IDLE_LOCATION);
            sleep(1);
        } while (false);
        sleep(1);
    }

    std::unique_ptr<sdk::LogicalAggStage> promote(
        const ::MongoExtensionCatalogContext& catalogContext) const override {
        std::thread idleThread(threadFunction);
        idleThread.join();

        return sdk::TestAstNode<sdk::shared_test_stages::TransformLogicalAggStage>::promote(
            catalogContext);
    }

    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        return std::make_unique<IdleThreadsAstNode>(getName(), _arguments);
    }
};

DEFAULT_PARSE_NODE(IdleThreads);

using IdleThreadsStageDescriptor = sdk::TestStageDescriptor<"$idleThreads",
                                                            IdleThreadsParseNode,
                                                            true /* ExpectEmptyStageDefinition */>;

DEFAULT_EXTENSION(IdleThreads)
REGISTER_EXTENSION(IdleThreadsExtension)
DEFINE_GET_EXTENSION()
