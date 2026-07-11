// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host_connector/adapter/resolved_namespace_adapter.h"

#include "mongo/db/extension/shared/byte_buf_utils.h"

namespace mongo::extension::host_connector {

ResolvedNamespaceAdapter::ResolvedNamespaceAdapter(
    std::string&& dbName,
    std::string&& viewName,
    std::vector<mongo::BSONObj>&& viewPipeline,
    std::vector<MongoExtensionByteView>&& viewPipelineByteViews)
    : _dbName(std::move(dbName)),
      _viewName(std::move(viewName)),
      _viewPipeline(std::move(viewPipeline)),
      _viewPipelineByteViews(std::move(viewPipelineByteViews)),
      _api({.viewNamespace = {stringViewAsByteView(_dbName), stringViewAsByteView(_viewName)},
            .viewPipelineLen = _viewPipeline.size(),
            .viewPipeline =
                _viewPipelineByteViews.empty() ? nullptr : _viewPipelineByteViews.data()}) {}

ResolvedNamespaceAdapter ResolvedNamespaceAdapter::fromResolvedNamespace(
    const ResolvedNamespace& view) {
    std::string dbStr = DatabaseNameUtil::serialize(view.getNamespace().dbName(),
                                                    SerializationContext::stateCommandRequest());

    std::vector<BSONObj> stages = view.getOriginalBson();
    std::vector<::MongoExtensionByteView> stageViews;
    stageViews.reserve(stages.size());
    for (const auto& obj : stages) {
        stageViews.push_back(objAsByteView(obj));
    }

    return ResolvedNamespaceAdapter(std::move(dbStr),
                                    std::string{view.getNamespace().coll()},
                                    std::move(stages),
                                    std::move(stageViews));
}
}  // namespace mongo::extension::host_connector
