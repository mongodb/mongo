/**
 * Copyright 2013 (c) 10gen Inc.
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
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

/**
 * This file declares optimizations available on Pipelines. For now they should be considered part
 * of Pipeline's implementation rather than it's interface.
 */

#pragma once

#include "mongo/db/pipeline/pipeline.h"

namespace mongo {
/**
 * This class holds optimizations applied to a shard Pipeline and a merger Pipeline.
 *
 * Each function has the same signature and takes two Pipelines, both as an in/out parameters.
 */
class Pipeline::Optimizations::Sharded {
public:
    /**
     * Moves everything before a splittable stage to the shards. If there
     * are no splittable stages, moves everything to the shards.
     *
     * It is not safe to call this optimization multiple times.
     *
     * NOTE: looks for SplittableDocumentSources and uses that API
     */
    static void findSplitPoint(Pipeline* shardPipe, Pipeline* mergePipe);

    /**
     * If the final stage on shards is to unwind an array, move that stage to the merger. This
     * cuts down on network traffic and allows us to take advantage of reduced copying in
     * unwind.
     */
    static void moveFinalUnwindFromShardsToMerger(Pipeline* shardPipe, Pipeline* mergePipe);

    /**
     * Adds a stage to the end of shardPipe explicitly requesting all fields that mergePipe
     * needs. This is only done if it heuristically determines that it is needed. This
     * optimization can reduce the amount of network traffic and can also enable the shards to
     * convert less source BSON into Documents.
     */
    static void limitFieldsSentFromShardsToMerger(Pipeline* shardPipe, Pipeline* mergePipe);
};
}  // namespace mongo
