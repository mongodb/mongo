// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/database_name_util.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/explain_utils.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string_view>

namespace mongo::extension::host {
/**
 * CatalogContext is a helper class which is used to provide the boundary type
 * ::MongoExtensionCatalogContext to extensions. The lifetime of the boundary type's views is bound
 * to this helper class.
 */
class CatalogContext {
public:
    CatalogContext(const ExpressionContext& expCtx)
        : _dbName(DatabaseNameUtil::serialize(expCtx.getNamespaceString().dbName(),
                                              SerializationContext::stateCommandRequest())),
          _collName(expCtx.getNamespaceString().coll()),
          _uuid(expCtx.getUUID().has_value() ? expCtx.getUUID()->toString() : ""),
          _inRouter(expCtx.getInRouter()),
          _willBeMerged(expCtx.getNeedsMerge() && !expCtx.getInRouter()),
          _verbosity(expCtx.getExplain()),
          _shardId([&]() -> std::string {
              auto* opCtx = expCtx.getOperationContext();
              auto* shardingState =
                  opCtx ? ShardingState::get(opCtx->getServiceContext()) : nullptr;
              return shardingState && shardingState->enabled() ? shardingState->shardId().toString()
                                                               : std::string{};
          }()),
          _api(::MongoExtensionNamespaceString(stringDataAsByteView(std::string_view(_dbName)),
                                               stringDataAsByteView(std::string_view(_collName))),
               stringDataAsByteView(std::string_view(_uuid)),
               _inRouter,
               convertHostVerbosityToExtVerbosity(_verbosity),
               stringDataAsByteView(std::string_view(_shardId)),
               _willBeMerged) {}

    ~CatalogContext() = default;

    const ::MongoExtensionCatalogContext& getAsBoundaryType() const {
        return _api;
    }

private:
    const std::string _dbName;
    const std::string _collName;
    const std::string _uuid;
    const bool _inRouter;
    const bool _willBeMerged;
    const boost::optional<ExplainOptions::Verbosity> _verbosity;
    const std::string _shardId;
    const ::MongoExtensionCatalogContext _api;
};
};  // namespace mongo::extension::host
