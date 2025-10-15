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
#pragma once

#include "mongo/db/extension/host/document_source_extension.h"
#include "mongo/db/extension/host_connector/handle/aggregation_stage/ast_node.h"
#include "mongo/db/extension/host_connector/handle/aggregation_stage/logical.h"
#include "mongo/db/extension/host_connector/handle/aggregation_stage/parse_node.h"
#include "mongo/db/extension/host_connector/handle/aggregation_stage/stage_descriptor.h"

namespace mongo::extension::host {

class DocumentSourceExtensionOptimizable : public DocumentSourceExtension {
public:
    // Direct construction of a source or transform extension.
    DocumentSourceExtensionOptimizable(StringData name,
                                       const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       Id id,
                                       BSONObj rawStage,
                                       host_connector::AggStageDescriptorHandle staticDescriptor)
        : DocumentSourceExtension(name, expCtx, id, rawStage, staticDescriptor),
          _logicalStage(validateAndCreateLogicalStage()) {}

    Value serialize(const SerializationOptions& opts) const override;

private:
    const host_connector::LogicalAggStageHandle _logicalStage;

    host_connector::LogicalAggStageHandle validateAndCreateLogicalStage() {
        std::vector<host_connector::VariantNodeHandle> expandedNodes = _parseNode.expand();

        tassert(11164400,
                str::stream() << "Source or transform stage " << _stageName
                              << " must expand into exactly one node.",
                expandedNodes.size() == 1);

        if (const auto* astNodeHandlePtr =
                std::get_if<host_connector::AggStageAstNodeHandle>(&expandedNodes[0])) {
            return astNodeHandlePtr->bind();
        } else {
            tasserted(11164401,
                      str::stream() << "Source or transform extension" << _stageName
                                    << " must expand into an AST node");
        }
    }
};

}  // namespace mongo::extension::host
