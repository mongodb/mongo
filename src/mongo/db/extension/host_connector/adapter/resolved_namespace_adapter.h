/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
