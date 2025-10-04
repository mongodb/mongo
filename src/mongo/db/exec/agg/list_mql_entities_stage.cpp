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

#include "mongo/db/exec/agg/list_mql_entities_stage.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_list_mql_entities.h"
#include "mongo/db/pipeline/document_source_list_mql_entities_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

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

ListMqlEntitiesStage::ListMqlEntitiesStage(StringData stageName,
                                           const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                           MqlEntityTypeEnum type)
    : ListMqlEntitiesStage(
          stageName, pExpCtx, type, /* docSourceParserMap */ DocumentSource::getParserMap()) {}

boost::intrusive_ptr<Stage> ListMqlEntitiesStage::create_forTest(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    MqlEntityTypeEnum type,
    const StringMap<DocumentSource::ParserRegistration>& parserRegistrationMap) {
    return new ListMqlEntitiesStage(stageName, pExpCtx, type, parserRegistrationMap);
}

ListMqlEntitiesStage::ListMqlEntitiesStage(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    MqlEntityTypeEnum type,
    const StringMap<DocumentSource::ParserRegistration>& docSourceParserMap)
    : Stage(stageName, pExpCtx), _type(type) {
    for (auto&& [stageName, _] : docSourceParserMap) {
        _results.push_back(stageName);
    }

    // Canonicalize output order of results. Sort in descending order so that we can use a cheap
    // 'pop_back()' to return the results in order.
    std::sort(_results.begin(), _results.end(), std::greater<>());
}

GetNextResult ListMqlEntitiesStage::doGetNext() {
    if (_results.empty()) {
        return GetNextResult::makeEOF();
    }
    auto res = Document(
        BSON("name" << _results.back() << kEntityTypeFieldName << MqlEntityType_serializer(_type)));
    _results.pop_back();
    return res;
}
}  // namespace exec::agg
}  // namespace mongo
