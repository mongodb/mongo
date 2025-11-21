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
#include "mongo/db/extension/host/document_source_extension.h"
#include "mongo/db/extension/host/document_source_extension_optimizable.h"
#include "mongo/db/extension/host_connector/adapter/query_shape_opts_adapter.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/ast_node.h"

namespace mongo::extension::host {

ALLOCATE_DOCUMENT_SOURCE_ID(extensionExpandable, DocumentSourceExtensionExpandable::id);

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceExtensionExpandable::expandImpl(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const AggStageParseNodeHandle& parseNodeHandle) {
    std::list<boost::intrusive_ptr<DocumentSource>> outExpanded;
    std::vector<VariantNodeHandle> expanded = parseNodeHandle.expand();

    helper::visitExpandedNodes(
        expanded,
        [&](const HostAggStageParseNode& host) {
            const BSONObj& bsonSpec = host.getBsonSpec();
            outExpanded.splice(outExpanded.end(), DocumentSource::parse(expCtx, bsonSpec));
        },
        [&](const AggStageParseNodeHandle& handle) {
            std::list<boost::intrusive_ptr<DocumentSource>> children = expandImpl(expCtx, handle);
            outExpanded.splice(outExpanded.end(), children);
        },
        [&](const HostAggStageAstNode& hostAst) {
            const BSONObj& bsonSpec = hostAst.getIdLookupSpec();
            outExpanded.splice(outExpanded.end(), DocumentSource::parse(expCtx, bsonSpec));
        },
        [&](AggStageAstNodeHandle handle) {
            outExpanded.emplace_back(
                DocumentSourceExtensionOptimizable::create(expCtx, std::move(handle)));
        });

    return outExpanded;
}

Value DocumentSourceExtensionExpandable::serialize(const SerializationOptions& opts) const {
    tassert(10978000,
            "SerializationOptions should change literals while represented as a "
            "DocumentSourceExtensionExpandable",
            !opts.isKeepingLiteralsUnchanged());

    host_connector::QueryShapeOptsAdapter adapter{&opts};
    return Value(_parseNode.getQueryShape(adapter));
}

DocumentSource::Id DocumentSourceExtensionExpandable::getId() const {
    return id;
}

Desugarer::StageExpander DocumentSourceExtensionExpandable::stageExpander =
    [](Desugarer* desugarer, DocumentSourceContainer::iterator itr, const DocumentSource& stage) {
        tassert(10978001, "Desugarer iterator does not match current stage", itr->get() == &stage);

        const auto& expandable = static_cast<const DocumentSourceExtensionExpandable&>(stage);
        std::list<boost::intrusive_ptr<DocumentSource>> expandedExtension = expandable.expand();
        tassert(10978002, "Expanded extension pipeline is empty", !expandedExtension.empty());

        // Replaces the extension stage with its expanded form and moves the iterator to *after* the
        // new stages added.
        return desugarer->replaceStageWith(itr, std::move(expandedExtension));
    };

/**
 * Register the stage expander only after DocumentSource ID allocation is complete. This ensures
 * that the expander is not registered under kUnallocatedId (0).
 */
MONGO_INITIALIZER_WITH_PREREQUISITES(RegisterStageExpanderForExtensionExpandable,
                                     ("EndDocumentSourceIdAllocation"))
(InitializerContext*) {
    tassert(11368000,
            "DocumentSourceExtensionExpandable::id must be allocated before registering expander",
            DocumentSourceExtensionExpandable::id != DocumentSource::kUnallocatedId);
    Desugarer::registerStageExpander(DocumentSourceExtensionExpandable::id,
                                     DocumentSourceExtensionExpandable::stageExpander);
}

}  // namespace mongo::extension::host
