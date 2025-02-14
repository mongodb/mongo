/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/pipeline/document_source_set_metadata.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/util/str.h"

namespace mongo::hybrid_scoring_util {

bool isScoreStage(const boost::intrusive_ptr<DocumentSource>& stage) {
    if (stage->getSourceName() != DocumentSourceSetMetadata::kStageName) {
        return false;
    }
    auto singleDocTransform = static_cast<DocumentSourceSingleDocumentTransformation*>(stage.get());
    auto setMetadataTransform =
        static_cast<SetMetadataTransformation*>(&singleDocTransform->getTransformer());
    return setMetadataTransform->getMetaType() == DocumentMetadataFields::MetaType::kScore;
}

// TODO SERVER-100754: A pipeline that begins with a $match stage that isTextQuery() should also
// count.
bool isScoredPipeline(const Pipeline& pipeline) {
    // Note that we don't check for $rankFusion and $scoreFusion explicitly because it will be
    // desugared by this point.
    static const std::set<StringData> implicitlyScoredStages{DocumentSourceVectorSearch::kStageName,
                                                             DocumentSourceSearch::kStageName};
    auto sources = pipeline.getSources();
    if (sources.empty()) {
        return false;
    }

    auto firstStageName = sources.front()->getSourceName();
    return implicitlyScoredStages.contains(firstStageName) ||
        std::any_of(sources.begin(), sources.end(), isScoreStage);
}

}  // namespace mongo::hybrid_scoring_util
