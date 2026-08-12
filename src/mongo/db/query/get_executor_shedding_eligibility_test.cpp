// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/memory_tracking/query_memory_load_shedding.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <mutex>
#include <string>

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.shedding");

/**
 * Covers where the find planning paths opt an operation into query-memory load shedding. Two
 * conditions must both hold: the running command must be 'find' (which keeps internal callers of
 * getExecutorFind() ineligible, since they run under some other command or none at all), and the
 * query must have missed the express fast path (an express plan builds no memory tracker, so it
 * could never be shed and the bookkeeping would be paid for something that cannot happen).
 *
 * Only the collection is needed -- the '_id' index it gets implicitly is what makes express
 * possible, and planning does not require documents.
 */
class GetExecutorSheddingEligibilityTest : public CatalogTestFixture {
protected:
    void setUp() final {
        CatalogTestFixture::setUp();
        ASSERT_OK(
            storageInterface()->createCollection(operationContext(), kNss, CollectionOptions()));
    }

    /**
     * Sets the CurOp's command to 'commandName', as command dispatch does, so the planning paths
     * can consult it. Passing nullptr models an internal caller running under no command at all.
     */
    void setCurOpCommand(const char* commandName) {
        Command* command =
            commandName ? CommandHelpers::findCommand(operationContext(), commandName) : nullptr;
        if (commandName) {
            ASSERT(command) << "command not registered in this unit test binary: " << commandName;
        }
        std::lock_guard<Client> clientLock(*operationContext()->getClient());
        CurOp::get(operationContext())
            ->setGenericOpRequestDetails(clientLock, kNss, command, BSONObj(), NetworkOp::dbQuery);
    }

    /**
     * Plans 'filter' through getExecutorFind() and returns the resulting executor's plan summary,
     * so a test can assert which path was actually taken rather than assuming it.
     *
     * The ExpressionContext is built here rather than in setUp() because the
     * 'featureFlagGetExecutorDeferredEngineChoice' value is snapshotted into its IfrContext at
     * construction; a ServerParameterGuard in the caller's scope must therefore already be active.
     */
    std::string planFind(BSONObj filter) {
        auto expCtx = make_intrusive<ExpressionContextForTest>(operationContext(), kNss);

        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest::fromOpCtx(
                operationContext(), kNss, AcquisitionPrerequisites::OperationType::kRead),
            MODE_IS);
        auto colls = MultipleCollectionAccessor(
            std::move(coll), {}, false /* isAnySecondaryNamespaceAViewOrNotFullyLocal */);

        auto findCommand = std::make_unique<FindCommandRequest>(kNss);
        findCommand->setFilter(filter);
        auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = expCtx,
            .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand)}});

        auto exec = getExecutorFind(
            operationContext(), colls, std::move(cq), PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);
        ASSERT_OK(exec.getStatus());
        return exec.getValue()->getPlanExplainer().getPlanSummary();
    }
};

// Two planning paths must each place the opt-in after their own express decision: getExecutorFind()
// itself, and getExecutorFindDeferredEngineChoice(), which it delegates to under
// 'featureFlagGetExecutorDeferredEngineChoice'. Tests asserting eligibility is *set* are split per
// path rather than looped, because eligibility persists on the operation's QueryLifespan once set
// -- a second loop iteration would pass trivially.

// An _id point query takes the express path, so it is left ineligible even though the command is
// find. Eligibility is never set here, so both paths can be exercised on one operation.
TEST_F(GetExecutorSheddingEligibilityTest, ExpressQueryIsNotMarkedEligible) {
    setCurOpCommand("find");
    for (bool deferredEnabled : {false, true}) {
        unittest::ServerParameterGuard deferredEngineChoice{
            "featureFlagGetExecutorDeferredEngineChoice", deferredEnabled};

        const auto summary = planFind(BSON("_id" << 7));
        ASSERT_STRING_CONTAINS(summary, "EXPRESS");
        ASSERT_FALSE(isOperationQueryMemorySheddingEligible(operationContext()))
            << "deferredEngineChoice=" << deferredEnabled;
    }
}

// A query that misses the express path is marked eligible when running under the find command.
TEST_F(GetExecutorSheddingEligibilityTest, NonExpressQueryIsMarkedEligibleLegacyPath) {
    setCurOpCommand("find");
    unittest::ServerParameterGuard deferredEngineChoice{
        "featureFlagGetExecutorDeferredEngineChoice", false};

    ASSERT_FALSE(isOperationQueryMemorySheddingEligible(operationContext()));
    const auto summary = planFind(BSON("a" << BSON("$gt" << 5)));
    ASSERT_STRING_OMITS(summary, "EXPRESS");
    ASSERT_TRUE(isOperationQueryMemorySheddingEligible(operationContext()));
}

TEST_F(GetExecutorSheddingEligibilityTest, NonExpressQueryIsMarkedEligibleDeferredPath) {
    setCurOpCommand("find");
    unittest::ServerParameterGuard deferredEngineChoice{
        "featureFlagGetExecutorDeferredEngineChoice", true};

    ASSERT_FALSE(isOperationQueryMemorySheddingEligible(operationContext()));
    const auto summary = planFind(BSON("a" << BSON("$gt" << 5)));
    ASSERT_STRING_OMITS(summary, "EXPRESS");
    ASSERT_TRUE(isOperationQueryMemorySheddingEligible(operationContext()));
}

// Internal callers reach getExecutorFind() under some command other than find -- or under none at
// all, as the resharding pipelines do -- and are never made shed-eligible, not even for a
// non-express plan that would otherwise qualify.
TEST_F(GetExecutorSheddingEligibilityTest, CallerWithoutFindCommandIsNeverMarkedEligible) {
    for (const char* commandName : {static_cast<const char*>(nullptr), "insert"}) {
        setCurOpCommand(commandName);
        for (bool deferredEnabled : {false, true}) {
            unittest::ServerParameterGuard deferredEngineChoice{
                "featureFlagGetExecutorDeferredEngineChoice", deferredEnabled};

            const auto summary = planFind(BSON("a" << BSON("$gt" << 5)));
            ASSERT_STRING_OMITS(summary, "EXPRESS");
            ASSERT_FALSE(isOperationQueryMemorySheddingEligible(operationContext()))
                << "command=" << (commandName ? commandName : "<none>")
                << " deferredEngineChoice=" << deferredEnabled;
        }
    }
}

}  // namespace
}  // namespace mongo
