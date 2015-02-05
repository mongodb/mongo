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

#include <deque>

#include <boost/intrusive_ptr.hpp>

#include "mongo/db/pipeline/value.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/timer.h"

namespace mongo {
    class BSONObj;
    class BSONObjBuilder;
    class Command;
    struct DepsTracker;
    class DocumentSource;
    struct ExpressionContext;
    class Privilege;

    /** mongodb "commands" (sent via db.$cmd.findOne(...))
        subclass to make a command.  define a singleton object for it.
        */
    class Pipeline :
        public IntrusiveCounterUnsigned {
    public:
        /**
         * Create a pipeline from the command.
         *
         * @param errmsg where to write errors, if there are any
         * @param cmdObj the command object sent from the client
         * @returns the pipeline, if created, otherwise a NULL reference
         */
        static boost::intrusive_ptr<Pipeline> parseCommand(
            std::string& errmsg,
            const BSONObj& cmdObj,
            const boost::intrusive_ptr<ExpressionContext>& pCtx);

        /// Helper to implement Command::addRequiredPrivileges
        static void addRequiredPrivileges(Command* commandTemplate,
                                          const std::string& dbname,
                                          BSONObj cmdObj,
                                          std::vector<Privilege>* out);

        const boost::intrusive_ptr<ExpressionContext>& getContext() const { return pCtx; }

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
          Write the Pipeline as a BSONObj command.  This should be the
          inverse of parseCommand().

          This is only intended to be used by the shard command obtained
          from splitForSharded().  Some pipeline operations in the merge
          process do not have equivalent command forms, and using this on
          the mongos Pipeline will cause assertions.

          @param the builder to write the command to
        */
        Document serialize() const;

        /** Stitch together the source pointers (by calling setSource) for each source in sources.
         *  Must be called after optimize and addInitialSource but before trying to get results.
         */
        void stitch();

        /**
          Run the Pipeline on the given source.

          @param result builder to write the result to
        */
        void run(BSONObjBuilder& result);

        bool isExplain() const { return explain; }

        /// The initial source is special since it varies between mongos and mongod.
        void addInitialSource(boost::intrusive_ptr<DocumentSource> source);

        /// The source that represents the output. Returns a non-owning pointer.
        DocumentSource* output() { invariant( !sources.empty() ); return sources.back().get(); }

        /// Returns true if this pipeline only uses features that work in mongos.
        bool canRunInMongos() const;

        /**
         * Write the pipeline's operators to a std::vector<Value>, with the
         * explain flag true (for DocumentSource::serializeToArray()).
         */
        std::vector<Value> writeExplainOps() const;
        
        /**
         * Returns the dependencies needed by this pipeline.
         *
         * initialQuery is used as a fallback for metadata dependency detection. The assumption is
         * that any metadata produced by the query is needed unless we can prove it isn't.
         */
        DepsTracker getDependencies(const BSONObj& initialQuery) const;

        /**
          The aggregation command name.
         */
        static const char commandName[];

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

        static const char pipelineName[];
        static const char explainName[];
        static const char fromRouterName[];
        static const char serverPipelineName[];
        static const char mongosPipelineName[];

        Pipeline(const boost::intrusive_ptr<ExpressionContext> &pCtx);

        typedef std::deque<boost::intrusive_ptr<DocumentSource> > SourceContainer;
        SourceContainer sources;
        bool explain;

        boost::intrusive_ptr<ExpressionContext> pCtx;
    };
} // namespace mongo
