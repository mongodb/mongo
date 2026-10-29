// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/version_context.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * An ExpressionContext with a default OperationContext that can have state (like the resolved
 * namespace map) manipulated after construction. In contrast, a regular ExpressionContext requires
 * the resolved namespaces to be provided on construction and does not allow them to be subsequently
 * mutated.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ExpressionContextForTest : public ExpressionContext {
public:
    static constexpr TimeZoneDatabase* kNullTimeZoneDatabase = nullptr;

    /**
     * If there is a global ServiceContext available, this constructor will adopt it. Otherwise, it
     * will internally create an owned QueryTestServiceContext. Similarly, if an OperationContext
     * already exists on the current client then it will be adopted, otherwise an owned OpCtx will
     * be created using the ServiceContext. The OpCtx will be set on the ExpressionContextForTest.
     * Defaults to using a namespace of "test.namespace".
     */
    ExpressionContextForTest()
        : ExpressionContextForTest(
              NamespaceString::createNamespaceString_forTest("test"sv, "namespace"sv)) {}
    /**
     * If there is a global ServiceContext available, this constructor will adopt it. Otherwise, it
     * will internally create an owned QueryTestServiceContext. Similarly, if an OperationContext
     * already exists on the current client then it will be adopted, otherwise an owned OpCtx will
     * be created using the ServiceContext. The OpCtx will be set on the ExpressionContextForTest.
     */
    ExpressionContextForTest(
        NamespaceString nss,
        multiversion::FeatureCompatibilityVersion fcv =
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion())
        : ExpressionContext(ExpressionContextParams{
              .vCtx = VersionContext(fcv),
              .ifrContext = std::make_shared<IncrementalFeatureRolloutContext>(),
              .ns = nss,
              .originalNs = nss,
              .runtimeConstants = LegacyRuntimeConstants(Date_t::now(), Timestamp(1, 0))}) {
        // If there is an existing global ServiceContext, adopt it. Otherwise, create a new context.
        // Similarly, we create a new OperationContext or adopt an existing context as appropriate.
        if (hasGlobalServiceContext()) {
            _serviceContext = getGlobalServiceContext();
            if (!Client::getCurrent()->getOperationContext()) {
                _testOpCtx = getGlobalServiceContext()->makeOperationContext(Client::getCurrent());
            }
        } else {
            _serviceContext = std::make_unique<QueryTestServiceContext>();
            _testOpCtx = get<std::unique_ptr<QueryTestServiceContext>>(_serviceContext)
                             ->makeOperationContext();
        }

        // Resolve the active OperationContext and set it on the ExpressionContextForTest.
        _params.opCtx = _testOpCtx ? _testOpCtx.get() : Client::getCurrent()->getOperationContext();
        // As we don't have an OperationContext or TimeZoneDatabase prior to base class
        // ExpressionContext construction, we must resolve one. If there exists a TimeZoneDatabase
        // associated with the current ServiceContext, adopt it. Otherwise, create a
        // new one.
        _setTimeZoneDatabase();

        if (_params.letParameters) {
            variables.seedVariablesWithLetParameters(
                this, *_params.letParameters, [](const Expression* expr) {
                    return expression::getDependencies(expr).hasNoRequirements();
                });
        }
    }

    /**
     * Constructor which sets the given OperationContext on the ExpressionContextForTest. This will
     * also resolve the ExpressionContextForTest's ServiceContext from the OperationContext.
     * Defaults to using a namespace of "test.namespace".
     */
    ExpressionContextForTest(OperationContext* opCtx)
        : ExpressionContextForTest(
              opCtx, NamespaceString::createNamespaceString_forTest("test"sv, "namespace"sv)) {}

    /**
     * Constructor which sets the given OperationContext on the ExpressionContextForTest. This will
     * also resolve the ExpressionContextForTest's ServiceContext from the OperationContext.
     */
    ExpressionContextForTest(
        OperationContext* opCtx,
        NamespaceString nss,
        multiversion::FeatureCompatibilityVersion fcv =
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion())
        : ExpressionContext(ExpressionContextParams{
              .opCtx = opCtx,
              .vCtx = VersionContext(fcv),
              .ifrContext = std::make_shared<IncrementalFeatureRolloutContext>(),
              .ns = nss,
              .originalNs = nss,
              .runtimeConstants = LegacyRuntimeConstants(Date_t::now(), Timestamp(1, 0))}),
          _serviceContext(opCtx->getServiceContext()) {
        // Resolve the TimeZoneDatabase to be used by this ExpressionContextForTest.
        _setTimeZoneDatabase();
    }

    /**
     * Constructor which sets the given OperationContext and SerializationContext on the
     * ExpressionContextForTest. This will also resolve the ExpressionContextForTest's
     * ServiceContext from the OperationContext.
     */
    ExpressionContextForTest(
        OperationContext* opCtx,
        NamespaceString nss,
        SerializationContext sc,
        multiversion::FeatureCompatibilityVersion fcv =
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion())
        : ExpressionContext(ExpressionContextParams{
              .opCtx = opCtx,
              .vCtx = VersionContext(fcv),
              .ifrContext = std::make_shared<IncrementalFeatureRolloutContext>(),
              .ns = nss,
              .originalNs = nss,
              .serializationContext = sc,
              .runtimeConstants = LegacyRuntimeConstants(Date_t::now(), Timestamp(1, 0)),
          }),
          _serviceContext(opCtx->getServiceContext()) {
        // Resolve the TimeZoneDatabase to be used by this ExpressionContextForTest.
        _setTimeZoneDatabase();
    }

    /**
     * Constructor which sets the given OperationContext on the ExpressionContextForTest. This will
     * also resolve the ExpressionContextForTest's ServiceContext from the OperationContext.
     */
    ExpressionContextForTest(
        OperationContext* opCtx,
        const AggregateCommandRequest& request,
        multiversion::FeatureCompatibilityVersion fcv =
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion())
        : ExpressionContext(ExpressionContextParams{
              .opCtx = opCtx,
              .vCtx = VersionContext(fcv),
              .ifrContext = std::make_shared<IncrementalFeatureRolloutContext>(),
              .ns = request.getNamespace(),
              .originalNs = request.getNamespace(),
              .serializationContext = request.getSerializationContext(),
              .runtimeConstants = request.getLegacyRuntimeConstants(),
              .letParameters = request.getLet(),
              .fromRouter = aggregation_request_helper::getFromRouter(request),
              .mergeType = request.getNeedsMerge()
                  ? (request.getNeedsSortedMerge() ? MergeType::sortedMerge
                                                   : MergeType::unsortedMerge)
                  : MergeType::noMerge,
              .forPerShardCursor = request.getPassthroughToShard().has_value(),
              .allowDiskUse = request.getAllowDiskUse().value_or(false),
              .bypassDocumentValidation = request.getBypassDocumentValidation().value_or(false),
              .isMapReduceCommand = request.getIsMapReduceCommand()}),
          _serviceContext(opCtx->getServiceContext()) {
        // Resolve the TimeZoneDatabase to be used by this ExpressionContextForTest.
        _setTimeZoneDatabase();
        // In cases where explain = true, use the kQueryPlanner verbosity.
        if (request.getExplain().get_value_or(false)) {
            setExplain(ExplainOptions::Verbosity::kQueryPlanner);
        }

        if (_params.letParameters) {
            variables.seedVariablesWithLetParameters(
                this, *_params.letParameters, [](const Expression* expr) {
                    return expression::getDependencies(expr).hasNoRequirements();
                });
        }
    }

    /**
     * Constructor which sets the given OperationContext on the ExpressionContextForTest. This will
     * also resolve the ExpressionContextForTest's ServiceContext from the OperationContext
     * and accepts letParameters.
     */
    ExpressionContextForTest(
        OperationContext* opCtx,
        const NamespaceString& nss,
        std::unique_ptr<CollatorInterface> collator,
        const boost::optional<BSONObj>& letParameters = boost::none,
        multiversion::FeatureCompatibilityVersion fcv =
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion())
        : ExpressionContext(ExpressionContextParams{
              .opCtx = opCtx,
              .vCtx = VersionContext(fcv),
              .ifrContext = std::make_shared<IncrementalFeatureRolloutContext>(),
              .collator = std::move(collator),
              .ns = nss,
              .originalNs = nss,
              .runtimeConstants = LegacyRuntimeConstants(Date_t::now(), Timestamp(1, 0)),
              .letParameters = letParameters}),
          _serviceContext(opCtx->getServiceContext()) {
        // Resolve the TimeZoneDatabase to be used by this ExpressionContextForTest.
        _setTimeZoneDatabase();

        if (_params.letParameters) {
            variables.seedVariablesWithLetParameters(
                this, *_params.letParameters, [](const Expression* expr) {
                    return expression::getDependencies(expr).hasNoRequirements();
                });
        }
    }

    /**
     * Sets the resolved definition for an involved namespace.
     */
    void setResolvedNamespace(const NamespaceString& nss, ResolvedNamespace resolvedNamespace) {
        _params.resolvedNamespaces[nss] = std::move(resolvedNamespace);
    }

    ServiceContext* getServiceContext() {
        struct Visitor {
            auto operator()(ServiceContext* ctx) {
                return ctx;
            }
            auto operator()(const std::unique_ptr<QueryTestServiceContext>& ctx) {
                return ctx->getServiceContext();
            }
        };
        return visit(Visitor{}, _serviceContext);
    }
    void setExplain(boost::optional<ExplainOptions::Verbosity> verbosity) {
        _params.explain = std::move(verbosity);
    }

private:
    // In cases when there is a ServiceContext, if there already exists a TimeZoneDatabase
    // associated with the ServiceContext, adopt it. Otherwise, create a new one.
    void _setTimeZoneDatabase() {
        auto* serviceContext = getServiceContext();
        if (!TimeZoneDatabase::get(serviceContext)) {
            TimeZoneDatabase::set(serviceContext, std::make_unique<TimeZoneDatabase>());
        }
        _params.timeZoneDatabase = TimeZoneDatabase::get(serviceContext);
    }

    std::variant<ServiceContext*, std::unique_ptr<QueryTestServiceContext>> _serviceContext;
    ServiceContext::UniqueOperationContext _testOpCtx;
};

}  // namespace mongo
