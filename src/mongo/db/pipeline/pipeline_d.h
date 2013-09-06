/**
 * Copyright 2012 (c) 10gen Inc.
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

#pragma once

#include "mongo/pch.h"

namespace mongo {
    class DocumentSourceCursor;
    struct ExpressionContext;
    class Pipeline;

    /*
      PipelineD is an extension of the Pipeline class, but with additional
      material that references symbols that are not available in mongos,
      where the remainder of the Pipeline class also functions.  PipelineD
      is a friend of Pipeline so that it can have equal access to Pipeline's
      members.

      See the friend declaration in Pipeline.
     */
    class PipelineD {
    public:

        /**
           Create a Cursor wrapped in a DocumentSourceCursor, which is suitable
           to be the first source for a pipeline to begin with.  This source
           will feed the execution of the pipeline.

           This method looks for early pipeline stages that can be folded into
           the underlying cursor, and when a cursor can absorb those, they
           are removed from the head of the pipeline.  For example, an
           early match can be removed and replaced with a Cursor that will
           do an index scan.

           The cursor is added to the front of the pipeline's sources.

           @param pPipeline the logical "this" for this operation
           @param dbName the name of the database
           @param pExpCtx the expression context for this pipeline
         */
        static void prepareCursorSource(
            const intrusive_ptr<Pipeline> &pPipeline,
            const string &dbName,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

    private:
        PipelineD(); // does not exist:  prevent instantiation
    };

} // namespace mongo
