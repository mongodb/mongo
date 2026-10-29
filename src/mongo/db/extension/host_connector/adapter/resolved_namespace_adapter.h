// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/util/modules.h"

namespace mongo::extension::host_connector {

/**
 * An adapter to convert from the internal view ResolvedNamespace representation to the
 * MongoExtensionResolvedNamespace struct that is passed to the extension. This class also owns the
 * memory for the view pipeline that is passed to the extension, since
 * MongoExtensionResolvedNamespace only holds a pointer to the pipeline stages.
 *
 * NOTE: The view pipeline is stored both as a vector of BSONObj and a vector of
 * MongoExtensionByteView because MongoExtensionResolvedNamespace requires the pipeline stages to be
 * passed as byte views, but we also want to keep the underlying BSONObj alive to ensure the byte
 * views remain valid.
 */
class ResolvedNamespaceAdapter {
public:
    static ResolvedNamespaceAdapter fromResolvedNamespace(const ResolvedNamespace& view);

    const ::MongoExtensionResolvedNamespace& getAsBoundaryType() const {
        return _api;
    }

private:
    ResolvedNamespaceAdapter(std::string&& dbName,
                             std::string&& viewName,
                             std::vector<mongo::BSONObj>&& viewPipeline,
                             std::vector<MongoExtensionByteView>&& viewPipelineByteViews);

    // ResolvedNamespaceAdapter is not copyable or movable since it owns the memory for the view
    // pipeline stages, so we want to avoid accidentally copying or moving and invalidating that
    // memory.
    ResolvedNamespaceAdapter(const ResolvedNamespaceAdapter&) = delete;
    ResolvedNamespaceAdapter(ResolvedNamespaceAdapter&&) = delete;
    ResolvedNamespaceAdapter& operator=(const ResolvedNamespaceAdapter&) = delete;
    ResolvedNamespaceAdapter& operator=(ResolvedNamespaceAdapter&&) = delete;

    const std::string _dbName;
    const std::string _viewName;
    const std::vector<BSONObj> _viewPipeline;
    const std::vector<MongoExtensionByteView> _viewPipelineByteViews;

    const ::MongoExtensionResolvedNamespace _api;
};
}  // namespace mongo::extension::host_connector
