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
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/timer.h"

namespace mongo {
class BSONObj;
class BSONObjBuilder;
class ClientBasic;
class CollatorInterface;
class DocumentSource;
struct ExpressionContext;
class OperationContext;

/**
 * A Pipeline object represents a list of DocumentSources and is responsible for optimizing the
 * pipeline.
 */
class Pipeline : public IntrusiveCounterUnsigned {
public:
    typedef std::list<boost::intrusive_ptr<DocumentSource>> SourceContainer;

    /**
     * Parses a Pipeline from a BSONElement representing a list of DocumentSources. Returns a non-OK
     * status if it failed to parse. The returned pipeline is not optimized, but the caller may
     * convert it to an optimized pipeline by calling optimizePipeline().
     */
    static StatusWith<boost::intrusive_ptr<Pipeline>> parse(
        const std::vector<BSONObj>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Creates a Pipeline from an existing SourceContainer.
     *
     * Returns a non-OK status if any stage is in an invalid position. For example, if an $out stage
     * is present but is not the last stage.
     */
    static StatusWith<boost::intrusive_ptr<Pipeline>> create(
        SourceContainer sources, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Helper to implement Command::checkAuthForCommand.
     */
    static Status checkAuthForCommand(ClientBasic* client,
                                      const std::string& dbname,
                                      const BSONObj& cmdObj);

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
      Split the current Pipeline into a Pipeline for each shard, and
      a Pipeline that combines the results within mongos.

      This permanently alters this pipeline for the merging operation.

      @returns the Spec for the pipeline command that should be sent
        to the shards
    */
    boost::intrusive_ptr<Pipeline> splitForSharded();

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
     * Propagates a reference to the ExpressionContext to all of the pipeline's contained stages and
     * expressions.
     */
    void injectExpressionContext(const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Returns any other collections involved in the pipeline in addition to the collection the
     * aggregation is run on.
     */
    std::vector<NamespaceString> getInvolvedCollections() const;

    /**
     * Serializes the pipeline into a form that can be parsed into an equivalent pipeline.
     */
    std::vector<Value> serialize() const;

    /**
      Run the Pipeline on the given source.

      @param result builder to write the result to
    */
    void run(BSONObjBuilder& result);

    /// The initial source is special since it varies between mongos and mongod.
    void addInitialSource(boost::intrusive_ptr<DocumentSource> source);

    /// The source that represents the output. Returns a non-owning pointer.
    DocumentSource* output() {
        invariant(!_sources.empty());
        return _sources.back().get();
    }

    /**
     * Write the pipeline's operators to a std::vector<Value>, with the
     * explain flag true (for DocumentSource::serializeToArray()).
     */
    std::vector<Value> writeExplainOps() const;

    /**
     * Returns the dependencies needed by this pipeline. 'metadataAvailable' should reflect what
     * metadata is present on documents that are input to the front of the pipeline.
     */
    DepsTracker getDependencies(DepsTracker::MetadataAvailable metadataAvailable) const;

    const SourceContainer& getSources() {
        return _sources;
    }

    /*
      PipelineD is a "sister" class that has additional functionality
      for the Pipeline.  It exists because of linkage requirements.
      Pipeline needs to function in mongod and mongos.  PipelineD
      contains extra functionality required in mongod, and which can't
      appear in mongos because the required symbols are unavailable
      for linking there.  Consider PipelineD to be an extension of this
      class for mongod only.
     */
    friend class PipelineD;

private:
    class Optimizations {
    public:
        // These contain static functions that optimize pipelines in various ways.
        // They are classes rather than namespaces so that they can be friends of Pipeline.
        // Classes are defined in pipeline_optimizations.h.
        class Local;
        class Sharded;
    };

    friend class Optimizations::Local;
    friend class Optimizations::Sharded;

    Pipeline(const boost::intrusive_ptr<ExpressionContext>& pCtx);
    Pipeline(SourceContainer stages, const boost::intrusive_ptr<ExpressionContext>& pCtx);

    /**
     * Stitch together the source pointers by calling setSource() for each source in '_sources'.
     * This function must be called any time the order of stages within the pipeline changes, e.g.
     * in optimizePipeline().
     */
    void stitch();

    /**
     * Returns a non-OK status if any stage is in an invalid position. For example, if an $out stage
     * is present but is not the last stage in the pipeline.
     */
    Status ensureAllStagesAreInLegalPositions() const;

    SourceContainer _sources;

    boost::intrusive_ptr<ExpressionContext> pCtx;
};
}  // namespace mongo
