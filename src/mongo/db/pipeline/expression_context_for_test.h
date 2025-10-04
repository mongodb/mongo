/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/version_context.h"

#include <boost/optional.hpp>

namespace mongo {

/**
 * An ExpressionContext with a default OperationContext that can have state (like the resolved
 * namespace map) manipulated after construction. In contrast, a regular ExpressionContext requires
 * the resolved namespaces to be provided on construction and does not allow them to be subsequently
 * mutated.
 */
class ExpressionContextForTest : public ExpressionContext {
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
              NamespaceString::createNamespaceString_forTest("test"_sd, "namespace"_sd)) {}
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
              opCtx, NamespaceString::createNamespaceString_forTest("test"_sd, "namespace"_sd)) {}

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
