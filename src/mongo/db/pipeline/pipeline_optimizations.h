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
     * This class holds optimizations applied to a single Pipeline.
     *
     * Each function has the same signature and takes a Pipeline as an in/out parameter.
     */
    class Pipeline::Optimizations::Local {
    public:
        /** 
         * Moves matches before any adjacent sort phases.
         *
         * This means we sort fewer items.  Neither sorts, nor matches (excluding $text)
         * change the documents in the stream, so this transformation shouldn't affect
         * the result.
         */
        static void moveMatchBeforeSort(Pipeline* pipeline);

        /**
         * Moves limits before any adjacent skip phases.
         *
         * This is more optimal for sharding since currently, we can only split
         * the pipeline at a single source and it is better to limit the results
         * coming from each shard. This also enables other optimizations like
         * coalescing the limit into a sort.
         */
        static void moveLimitBeforeSkip(Pipeline* pipeline);

        /**
         * Runs through the DocumentSources, and give each one the opportunity
         * to coalesce with its successor. If successful, remove the successor.
         *
         * This should generally be run after optimizations that reorder stages
         * to be most effective.
         *
         * NOTE: uses the DocumentSource::coalesce() method
         */
        static void coalesceAdjacent(Pipeline* pipeline);

        /**
         * Gives each DocumentSource the opportunity to optimize itself.
         *
         * NOTE: uses the DocumentSource::optimize() method
         */
        static void optimizeEachDocumentSource(Pipeline* pipeline);

        /**
         * Optimizes [$redact, $match] to [$match, $redact, $match] if possible.
         *
         * This gives us the ability to use indexes and reduce the number of
         * BSONObjs converted to Documents.
         */
        static void duplicateMatchBeforeInitalRedact(Pipeline* pipeline);
    };

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
    };
} // namespace mongo
