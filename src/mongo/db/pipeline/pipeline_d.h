/**
 * Copyright (C) 2012-2014 MongoDB Inc.
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

#include <boost/intrusive_ptr.hpp>
#include <memory>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"

namespace mongo {
class Collection;
class DocumentSourceCursor;
class DocumentSourceSort;
struct ExpressionContext;
class OperationContext;
class Pipeline;
class PlanExecutor;
struct PlanSummaryStats;
class BSONObj;
struct DepsTracker;

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
     * Create a Cursor wrapped in a DocumentSourceCursor, which is suitable
     * to be the first source for a pipeline to begin with.  This source
     * will feed the execution of the pipeline.
     *
     * This method looks for early pipeline stages that can be folded into
     * the underlying cursor, and when a cursor can absorb those, they
     * are removed from the head of the pipeline.  For example, an
     * early match can be removed and replaced with a Cursor that will
     * do an index scan.
     *
     * The cursor is added to the front of the pipeline's sources.
     *
     * Must have a AutoGetCollectionForRead before entering.
     *
     * If the returned PlanExecutor is non-null, you are responsible for ensuring
     * it receives appropriate invalidate and kill messages.
     *
     * @param pPipeline the logical "this" for this operation
     * @param pExpCtx the expression context for this pipeline
     */
    static std::shared_ptr<PlanExecutor> prepareCursorSource(
        OperationContext* txn,
        Collection* collection,
        const NamespaceString& nss,
        const boost::intrusive_ptr<Pipeline>& pPipeline,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    static std::string getPlanSummaryStr(const boost::intrusive_ptr<Pipeline>& pPipeline);

    static void getPlanSummaryStats(const boost::intrusive_ptr<Pipeline>& pPipeline,
                                    PlanSummaryStats* statsOut);

private:
    PipelineD();  // does not exist:  prevent instantiation

    /**
     * Creates a PlanExecutor to be used in the initial cursor source. If the query system can use
     * an index to provide a more efficient sort or projection, the sort and/or projection will be
     * incorporated into the PlanExecutor.
     *
     * 'sortObj' will be set to an empty object if the query system cannot provide a non-blocking
     * sort, and 'projectionObj' will be set to an empty object if the query system cannot provide a
     * covered projection.
     */
    static std::shared_ptr<PlanExecutor> prepareExecutor(
        OperationContext* txn,
        Collection* collection,
        const NamespaceString& nss,
        const boost::intrusive_ptr<Pipeline>& pipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const boost::intrusive_ptr<DocumentSourceSort>& sortStage,
        const DepsTracker& deps,
        const BSONObj& queryObj,
        BSONObj* sortObj,
        BSONObj* projectionObj);

    /**
     * Creates a DocumentSourceCursor from the given PlanExecutor and adds it to the front of the
     * Pipeline.
     */
    static std::shared_ptr<PlanExecutor> addCursorSource(
        const boost::intrusive_ptr<Pipeline>& pipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        std::shared_ptr<PlanExecutor> exec,
        DepsTracker deps,
        const BSONObj& queryObj = BSONObj(),
        const BSONObj& sortObj = BSONObj(),
        const BSONObj& projectionObj = BSONObj());
};

}  // namespace mongo
