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

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/pipeline/document_source_rank_fusion_inputs_gen.h"

namespace mongo::hybrid_scoring_util {
/**
 * Checks if this pipeline will generate score metadata.
 *
 * TODO SERVER-100394 This custom logic should be able to be replaced by using DepsTracker to
 * walk the pipeline and see if "score" metadata is produced.
 */
bool isScoredPipeline(const Pipeline& pipeline);

/**
 * Returns no error if the BSON pipeline is a selection pipeline. A selection pipeline only
 * retrieves a set of documents from a collection, without doing any modifications. For example it
 * cannot do a $project or $replaceRoot.
 */
Status isSelectionPipeline(const std::vector<BSONObj>& bsonPipeline);

/**
 * Returns no error if the BSON stage is a selection stage. A selection stage only retrieves a set
 * of documents from a collection, without doing any modifications.
 */
Status isSelectionStage(const BSONObj& bsonStage);

/**
 * Returns no error if the BSON pipeline is a ranked pipeline. A ranked pipeline is a pipeline that
 * starts with an implicitly ranked stage, or contains an explicit $sort.
 */
Status isRankedPipeline(const std::vector<BSONObj>& bsonPipeline);

/**
 * Returns no error if the BSON pipeline is an scored pipeline. An ordered pipeline is a pipeline
 * that begins with a stage that generates a score, or contains an explicit $score.
 */
Status isScoredPipeline(const std::vector<BSONObj>& bsonPipeline,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx);

/**
 * Returns true if the BSON contains a $rankFusion and false otherwise.
 */
bool isHybridSearchPipeline(const std::vector<BSONObj>& bsonPipeline);
}  // namespace mongo::hybrid_scoring_util
