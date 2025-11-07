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

#include "mongo/db/extension/host/document_source_extension_expandable.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/extension/host/aggregation_stage/ast_node.h"
#include "mongo/db/extension/host/aggregation_stage/parse_node.h"
#include "mongo/db/extension/host/document_source_extension_optimizable.h"
#include "mongo/db/extension/host_connector/query_shape_opts_adapter.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/ast_node.h"

namespace mongo::extension::host {

ALLOCATE_DOCUMENT_SOURCE_ID(extensionExpandable, DocumentSourceExtensionExpandable::id);

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceExtensionExpandable::expandImpl(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const AggStageParseNodeHandle& parseNodeHandle) {
    std::list<boost::intrusive_ptr<DocumentSource>> outExpanded;
    std::vector<VariantNodeHandle> expanded = parseNodeHandle.expand();

    for (auto& variantNodeHandle : expanded) {
        std::visit(
            [&](auto&& handle) {
                using H = std::decay_t<decltype(handle)>;

                // Case 1: Parse node handle.
                //   a) Host-allocated parse node: convert directly to a host
                //      DocumentSource using the host-provided BSON spec. No recursion
                //      in this branch.
                //   b) Extension-allocated parse node: Recurse on the parse node
                //      handle, splicing the results of its expansion.
                if constexpr (std::is_same_v<H, AggStageParseNodeHandle>) {
                    if (HostAggStageParseNode::isHostAllocated(*handle.get())) {
                        const BSONObj& bsonSpec =
                            static_cast<host::HostAggStageParseNode*>(handle.get())->getBsonSpec();
                        outExpanded.splice(outExpanded.end(),
                                           DocumentSource::parse(expCtx, bsonSpec));
                    } else {
                        std::list<boost::intrusive_ptr<DocumentSource>> children =
                            expandImpl(expCtx, handle);
                        outExpanded.splice(outExpanded.end(), children);
                    }
                }
                // Case 2: AST node handle.
                //   a) Host-allocated AST node: convert directly to a host DocumentSource using
                //      the host-provided BSON spec.
                //   b) Extension-allocated AST node: Construct a
                //      DocumentSourceExtensionOptimizable and release the AST node handle.
                else if constexpr (std::is_same_v<H, AggStageAstNodeHandle>) {
                    if (HostAggStageAstNode::isHostAllocated(*handle.get())) {
                        const BSONObj& bsonSpec =
                            static_cast<host::HostAggStageAstNode*>(handle.get())
                                ->getIdLookupSpec();
                        outExpanded.splice(outExpanded.end(),
                                           DocumentSource::parse(expCtx, bsonSpec));
                    } else {
                        outExpanded.emplace_back(
                            DocumentSourceExtensionOptimizable::create(expCtx, std::move(handle)));
                    }
                }
            },
            variantNodeHandle);
    }

    return outExpanded;
}

Value DocumentSourceExtensionExpandable::serialize(const SerializationOptions& opts) const {
    // TODO SERVER-109780: Remove this block and add an assert to check that only the query shape is
    // requested at this point.
    if (opts.isKeepingLiteralsUnchanged()) {
        // Convert to Optimizable representation to get the non-query shape serialization.
        auto expanded = expandImpl(getExpCtx(), _parseNode);

        std::vector<Value> tmp;
        expanded.front()->serializeToArray(tmp, opts);
        return std::move(tmp.front());
    }

    host_connector::QueryShapeOptsAdapter adapter{&opts};
    return Value(_parseNode.getQueryShape(adapter));
}

DocumentSource::Id DocumentSourceExtensionExpandable::getId() const {
    return id;
}

}  // namespace mongo::extension::host
