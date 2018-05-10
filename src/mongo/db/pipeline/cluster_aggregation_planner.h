/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/db/pipeline/pipeline.h"

namespace mongo {
namespace cluster_aggregation_planner {

/**
 * Performs optimizations with the aim of reducing computing time and network traffic when a
 * pipeline has been split into two pieces. Modifies 'shardPipeline' and 'mergingPipeline' such that
 * they may contain different stages, but still compute the same results when executed.
 */
void performSplitPipelineOptimizations(Pipeline* shardPipeline, Pipeline* mergingPipeline);

/**
 * Rips off an initial $sort stage that can be handled by cursor merging machinery. Returns the
 * sort key pattern of such a $sort stage if there was one, and boost::none otherwise.
 */
boost::optional<BSONObj> popLeadingMergeSort(Pipeline* mergePipeline);

/**
 * Creates a new DocumentSourceMergeCursors from the provided 'remoteCursors' and adds it to the
 * front of 'mergePipeline'.
 */
void addMergeCursorsSource(Pipeline* mergePipeline,
                           std::vector<RemoteCursor> remoteCursors,
                           executor::TaskExecutor*);

}  // namespace cluster_aggregation_planner
}  // namespace mongo
