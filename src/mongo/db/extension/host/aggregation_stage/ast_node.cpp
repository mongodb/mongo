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

#include "mongo/db/extension/host/aggregation_stage/ast_node.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/search/lite_parsed_internal_search_id_lookup.h"
#include "mongo/util/assert_util.h"

namespace mongo::extension::host {

std::unique_ptr<LiteParsedDocumentSource> AggStageAstNode::expandToLiteParsed(
    const NamespaceString& nss, const LiteParserOptions& options) const {
    auto lpds = LiteParsedDocumentSource::parse(nss, buildStageBson(), options);
    lpds->makeOwned();
    return lpds;
}

std::list<boost::intrusive_ptr<DocumentSource>> AggStageAstNode::expandToDocumentSource(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    return DocumentSource::parse(expCtx, buildStageBson());
}

IdLookupAstNode::IdLookupAstNode(std::unique_ptr<LiteParsedInternalSearchIdLookUp> lp)
    : _liteParsed(std::move(lp)) {}

const std::string& IdLookupAstNode::getName() const {
    return _liteParsed->getParseTimeName();
}

BSONObj IdLookupAstNode::buildStageBson() const {
    return BSON(getName() << _liteParsed->getSpec().toBSON());
}

std::unique_ptr<AggStageAstNode> IdLookupAstNode::clone() const {
    return std::make_unique<IdLookupAstNode>(
        std::make_unique<LiteParsedInternalSearchIdLookUp>(_liteParsed->getSpec()));
}

DocumentResultsAndMetadataAstNode::DocumentResultsAndMetadataAstNode(BSONObj stageBson)
    : _stageName(stageBson.firstElementFieldName()), _stageBson(stageBson.getOwned()) {}

const std::string& DocumentResultsAndMetadataAstNode::getName() const {
    return _stageName;
}

BSONObj DocumentResultsAndMetadataAstNode::buildStageBson() const {
    return _stageBson;
}

std::unique_ptr<AggStageAstNode> DocumentResultsAndMetadataAstNode::clone() const {
    return std::make_unique<DocumentResultsAndMetadataAstNode>(_stageBson);
}

}  // namespace mongo::extension::host
