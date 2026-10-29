// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/list_mql_entities_stage.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_list_mql_entities.h"
#include "mongo/db/pipeline/document_source_list_mql_entities_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
boost::intrusive_ptr<exec::agg::Stage> documentSourceListMqlEntitiesToStage(
    const boost::intrusive_ptr<DocumentSource>& documentSourceListMqlEntities) {
    auto* ptr = dynamic_cast<DocumentSourceListMqlEntities*>(documentSourceListMqlEntities.get());
    tassert(10886200, "expected 'DocumentSourceListMqlEntities' type", ptr);
    return make_intrusive<exec::agg::ListMqlEntitiesStage>(
        ptr->kStageName, ptr->getExpCtx(), ptr->getType());
}

namespace exec::agg {
REGISTER_AGG_STAGE_MAPPING(listMqlEntitiesStage,
                           DocumentSourceListMqlEntities::id,
                           documentSourceListMqlEntitiesToStage);

ListMqlEntitiesStage::ListMqlEntitiesStage(std::string_view stageName,
                                           const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                           MqlEntityTypeEnum type)
    : ListMqlEntitiesStage(stageName,
                           pExpCtx,
                           type,
                           /* docSourceParserMap */ LiteParsedDocumentSource::getParserMap()) {}

boost::intrusive_ptr<Stage> ListMqlEntitiesStage::create_forTest(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    MqlEntityTypeEnum type,
    const LiteParsedDocumentSource::ParserMap& parserRegistrationMap) {
    return new ListMqlEntitiesStage(stageName, pExpCtx, type, parserRegistrationMap);
}

ListMqlEntitiesStage::ListMqlEntitiesStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    MqlEntityTypeEnum type,
    const LiteParsedDocumentSource::ParserMap& docSourceParserMap)
    : Stage(stageName, pExpCtx), _type(type) {
    for (auto&& [stageName, registration] : docSourceParserMap) {
        // Exclude this stage if it is a stub that has no primary parser.
        if (registration.isExecutable()) {
            _results.push_back(stageName);
        }
    }

    // Canonicalize output order of results. Sort in descending order so that we can use a cheap
    // 'pop_back()' to return the results in order.
    std::sort(_results.begin(), _results.end(), std::greater<>());
}

GetNextResult ListMqlEntitiesStage::doGetNext() {
    if (_results.empty()) {
        return GetNextResult::makeEOF();
    }
    auto res =
        Document(BSON("name" << _results.back() << kEntityTypeFieldName << idl::serialize(_type)));
    _results.pop_back();
    return res;
}
}  // namespace exec::agg
}  // namespace mongo
