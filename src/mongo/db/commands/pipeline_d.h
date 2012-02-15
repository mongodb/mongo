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
 */

#pragma once

#include "pch.h"

namespace mongo {
    class DocumentSource;
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
           Create a Cursor wrapped in a DocumentSource, which is suitable
           to be the first source for a pipeline to begin with.  This source
           will feed the execution of the pipeline.

           This method looks for early pipeline stages that can be folded into
           the underlying cursor, and when a cursor can absorb those, they
           are removed from the head of the pipeline.  For example, an
           early match can be removed and replaced with a Cursor that will
           do an index scan.

           @param pPipeline the logical "this" for this operation
           @param dbName the name of the database
           @param pExpCtx the expression context for this pipeline
           @returns a document source that wraps an appropriate cursor to
             be at the beginning of this pipeline
         */
        static intrusive_ptr<DocumentSource> prepareCursorSource(
            const intrusive_ptr<Pipeline> &pPipeline,
            const string &dbName,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

    private:
        PipelineD(); // does not exist:  prevent instantiation
    };

} // namespace mongo
