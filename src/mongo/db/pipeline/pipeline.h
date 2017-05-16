/**
 * Copyright 2011 (c) 10gen Inc.
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

#include <list>
#include <vector>

#include <boost/intrusive_ptr.hpp>

#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/timer.h"

namespace mongo {
class BSONObj;
class BSONObjBuilder;
class ExpressionContext;
class DocumentSource;
class CollatorInterface;
class OperationContext;

/**
 * A Pipeline object represents a list of DocumentSources and is responsible for optimizing the
 * pipeline.
 */
class Pipeline {
public:
    typedef std::list<boost::intrusive_ptr<DocumentSource>> SourceContainer;

    /**
     * This class will ensure a Pipeline is disposed before it is deleted.
     */
    class Deleter {
    public:
        /**
         * Constructs an empty deleter. Useful for creating a
         * unique_ptr<Pipeline, Pipeline::Deleter> without populating it.
         */
        Deleter() {}

        explicit Deleter(OperationContext* opCtx) : _opCtx(opCtx) {}

        /**
         * If an owner of a std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> wants to assume
         * responsibility for calling PlanExecutor::dispose(), they can call dismissDisposal(). If
         * dismissed, a Deleter will not call dispose() when deleting the PlanExecutor.
         */
        void dismissDisposal() {
            _dismissed = true;
        }

        /**
         * Calls dispose() on 'pipeline', unless this Deleter has been dismissed.
         */
        void operator()(Pipeline* pipeline) {
            // It is illegal to call this method on a default-constructed Deleter.
            invariant(_opCtx);
            if (!_dismissed) {
                pipeline->dispose(_opCtx);
            }
            delete pipeline;
        }

    private:
        OperationContext* _opCtx = nullptr;

        bool _dismissed = false;
    };

    /**
     * Parses a Pipeline from a BSONElement representing a list of DocumentSources. Returns a non-OK
     * status if it failed to parse. The returned pipeline is not optimized, but the caller may
     * convert it to an optimized pipeline by calling optimizePipeline().
     *
     * It is illegal to create a pipeline using an ExpressionContext which contains a collation that
     * will not be used during execution of the pipeline. Doing so may cause comparisons made during
     * parse-time to return the wrong results.
     */
    static StatusWith<std::unique_ptr<Pipeline, Pipeline::Deleter>> parse(
        const std::vector<BSONObj>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Creates a Pipeline from an existing SourceContainer.
     *
     * Returns a non-OK status if any stage is in an invalid position. For example, if an $out stage
     * is present but is not the last stage.
     */
    static StatusWith<std::unique_ptr<Pipeline, Pipeline::Deleter>> create(
        SourceContainer sources, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Returns true if the provided aggregation command has a $out stage.
     */
    static bool aggSupportsWriteConcern(const BSONObj& cmd);

    const boost::intrusive_ptr<ExpressionContext>& getContext() const {
        return pCtx;
    }

    /**
     * Sets the OperationContext of 'pCtx' to nullptr.
     *
     * The PipelineProxyStage is responsible for detaching the OperationContext and releasing any
     * storage-engine state of the DocumentSourceCursor that may be present in '_sources'.
     */
    void detachFromOperationContext();

    /**
     * Sets the OperationContext of 'pCtx' to 'opCtx'.
     *
     * The PipelineProxyStage is responsible for reattaching the OperationContext and reacquiring
     * any storage-engine state of the DocumentSourceCursor that may be present in '_sources'.
     */
    void reattachToOperationContext(OperationContext* opCtx);

    /**
     * Releases any resources held by this pipeline such as PlanExecutors or in-memory structures.
     * Must be called before deleting a Pipeline.
     *
     * There are multiple cleanup scenarios:
     *  - This Pipeline will only ever use one OperationContext. In this case the Pipeline::Deleter
     *    will automatically call dispose() before deleting the Pipeline, and the owner need not
     *    call dispose().
     *  - This Pipeline may use multiple OperationContexts over its lifetime. In this case it
     *    is the owner's responsibility to call dispose() with a valid OperationContext before
     *    deleting the Pipeline.
     */
    void dispose(OperationContext* opCtx);

    /**
     * Split the current Pipeline into a Pipeline for each shard, and a Pipeline that combines the
     * results within mongos. This permanently alters this pipeline for the merging operation, and
     * returns a Pipeline object that should be executed on each targeted shard.
    */
    std::unique_ptr<Pipeline, Pipeline::Deleter> splitForSharded();

    /** If the pipeline starts with a $match, return its BSON predicate.
     *  Returns empty BSON if the first stage isn't $match.
     */
    BSONObj getInitialQuery() const;

    /**
     * Returns whether or not any DocumentSource in the pipeline needs the primary shard.
     */
    bool needsPrimaryShardMerger() const;

    /**
     * Modifies the pipeline, optimizing it by combining and swapping stages.
     */
    void optimizePipeline();

    /**
     * Returns any other collections involved in the pipeline in addition to the collection the
     * aggregation is run on.
     */
    std::vector<NamespaceString> getInvolvedCollections() const;

    /**
     * Serializes the pipeline into a form that can be parsed into an equivalent pipeline.
     */
    std::vector<Value> serialize() const;

    /// The initial source is special since it varies between mongos and mongod.
    void addInitialSource(boost::intrusive_ptr<DocumentSource> source);

    /**
     * Returns the next result from the pipeline, or boost::none if there are no more results.
     */
    boost::optional<Document> getNext();

    /**
     * Write the pipeline's operators to a std::vector<Value>, providing the level of detail
     * specified by 'verbosity'.
     */
    std::vector<Value> writeExplainOps(ExplainOptions::Verbosity verbosity) const;

    /**
     * Returns the dependencies needed by this pipeline. 'metadataAvailable' should reflect what
     * metadata is present on documents that are input to the front of the pipeline.
     */
    DepsTracker getDependencies(DepsTracker::MetadataAvailable metadataAvailable) const;

    const SourceContainer& getSources() {
        return _sources;
    }

    /**
     * PipelineD is a "sister" class that has additional functionality for the Pipeline. It exists
     * because of linkage requirements. Pipeline needs to function in mongod and mongos. PipelineD
     * contains extra functionality required in mongod, and which can't appear in mongos because the
     * required symbols are unavailable for linking there. Consider PipelineD to be an extension of
     * this class for mongod only.
     */
    friend class PipelineD;

private:
    class Optimizations {
    public:
        // This contains static functions that optimize pipelines in various ways.
        // This is a class rather than a namespace so that it can be a friend of Pipeline.
        // It is defined in pipeline_optimizations.h.
        class Sharded;
    };

    friend class Optimizations::Sharded;

    Pipeline(const boost::intrusive_ptr<ExpressionContext>& pCtx);
    Pipeline(SourceContainer stages, const boost::intrusive_ptr<ExpressionContext>& pCtx);

    ~Pipeline();

    /**
     * Stitch together the source pointers by calling setSource() for each source in '_sources'.
     * This function must be called any time the order of stages within the pipeline changes, e.g.
     * in optimizePipeline().
     */
    void stitch();

    /**
     * Reset all stages' child pointers to nullptr. Used to prevent dangling pointers during the
     * optimization process, where we might swap or destroy stages.
     */
    void unstitch();

    /**
     * Returns a non-OK status if any stage is in an invalid position. For example, if an $out stage
     * is present but is not the last stage in the pipeline.
     */
    Status ensureAllStagesAreInLegalPositions() const;

    SourceContainer _sources;

    boost::intrusive_ptr<ExpressionContext> pCtx;
    bool _disposed = false;
};
}  // namespace mongo
