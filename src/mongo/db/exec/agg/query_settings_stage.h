// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
using namespace std::literals::string_view_literals;
using QueryShapeConfigurationMap = stdx::unordered_map<query_shape::QueryShapeHash,
                                                       query_settings::QueryShapeConfiguration,
                                                       QueryShapeHashHasher>;

class QuerySettingsStage final : public Stage {
public:
    static constexpr std::string_view kDebugQueryShapeFieldName = "debugQueryShape"sv;

    /**
     * The possible states of the stage. First we are reading all the 'config.representativeQueries'
     * collection and return all QueryShapeConfigurations that have representative queries. Once the
     * cursor is exhausted, we iterate over the remaining configurations in the
     * '_queryShapeConfigsMap'.
     */
    enum class State {
        kReadingFromQueryShapeRepresentativeQueriesColl,
        kReadingFromQueryShapeConfigsMap
    };

    QuerySettingsStage(std::string_view stageName,
                       const boost::intrusive_ptr<ExpressionContext>& pExtCtx,
                       bool showDebugQueryShape);

    void detachFromOperationContext() final;
    void reattachToOperationContext(OperationContext* opCtx) final;

private:
    GetNextResult doGetNext() final;
    void doDispose() final;

    /**
     * DocumentSource that holds a cursor over the 'queryShapeRepresentativeQueries' collection:
     * - either on local node if we are in the replica set
     * - or on the configsvr if we are in the sharded cluster deployment.
     *
     * If gFeatureFlagPQSBackfill is disabled, returns empty DocumentSourceQueue.
     */
    boost::intrusive_ptr<DocumentSource> createQueryShapeRepresentativeQueriesCursor(
        OperationContext* opCtx);

    boost::intrusive_ptr<exec::agg::Stage> _queryShapeRepresentativeQueriesCursor;
    QueryShapeConfigurationMap _queryShapeConfigsMap;
    QueryShapeConfigurationMap::const_iterator _iterator;
    bool _showDebugQueryShape;
    State _state = State::kReadingFromQueryShapeRepresentativeQueriesColl;
};

}  // namespace mongo::exec::agg
