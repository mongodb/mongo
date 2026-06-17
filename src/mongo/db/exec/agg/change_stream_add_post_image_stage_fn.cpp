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

#include "mongo/db/exec/agg/change_stream_add_post_image_stage.h"
#include "mongo/db/exec/agg/change_stream_update_lookup_stage.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/single_doc_lookup/aggregation_single_document_lookup_executor.h"
#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/util/assert_util.h"

#include <memory>

namespace mongo {

/**
 * Builds the execution stage for DocumentSourceChangeStreamAddPostImage. The post-image work is
 * split across two single-responsibility stages selected by the 'fullDocument' mode:
 * - 'updateLookup'                -> ChangeStreamUpdateLookupStage (look up the current document).
 * - 'required' / 'whenAvailable'  -> ChangeStreamAddPostImageStage (compute the post-image from the
 *                                    pre-image plus the oplog update modification).
 */
boost::intrusive_ptr<exec::agg::Stage> documentSourceChangeStreamAddPostImageToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* changeStreamAddPostImageDS =
        dynamic_cast<DocumentSourceChangeStreamAddPostImage*>(documentSource.get());

    tassert(10561301,
            "expected 'DocumentSourceChangeStreamAddPostImage' type",
            changeStreamAddPostImageDS);

    if (changeStreamAddPostImageDS->isUpdateLookup()) {
        return make_intrusive<exec::agg::ChangeStreamUpdateLookupStage>(
            changeStreamAddPostImageDS->kStageName,
            changeStreamAddPostImageDS->getExpCtx(),
            std::make_unique<exec::agg::AggregationSingleDocumentLookupExecutor>());
    }

    return make_intrusive<exec::agg::ChangeStreamAddPostImageStage>(
        changeStreamAddPostImageDS->kStageName,
        changeStreamAddPostImageDS->getExpCtx(),
        changeStreamAddPostImageDS->getFullDocument());
}

namespace exec::agg {
REGISTER_AGG_STAGE_MAPPING(_internalChangeStreamAddPostImage,
                           DocumentSourceChangeStreamAddPostImage::id,
                           documentSourceChangeStreamAddPostImageToStageFn)

}  // namespace exec::agg
}  // namespace mongo
