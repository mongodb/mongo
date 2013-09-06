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

#include "mongo/pch.h"

#include "mongo/db/pipeline/value.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/timer.h"

namespace mongo {
    class BSONObj;
    class BSONObjBuilder;
    class BSONArrayBuilder;
    class DocumentSource;
    class DocumentSourceProject;
    class Expression;
    struct ExpressionContext;
    class ExpressionNary;
    struct OpDesc; // local private struct
    class Privilege;

    /** mongodb "commands" (sent via db.$cmd.findOne(...))
        subclass to make a command.  define a singleton object for it.
        */
    class Pipeline :
        public IntrusiveCounterUnsigned {
    public:
        /**
          Create a pipeline from the command.

          @param errmsg where to write errors, if there are any
          @param cmdObj the command object sent from the client
          @returns the pipeline, if created, otherwise a NULL reference
         */
        static intrusive_ptr<Pipeline> parseCommand(
            string &errmsg, BSONObj &cmdObj,
            const intrusive_ptr<ExpressionContext> &pCtx);

        /// Helper to implement Command::addRequiredPrivileges
        static void addRequiredPrivileges(const string& inputNs,
                                          BSONObj cmdObj,
                                          vector<Privilege>* out);

        /**
          Get the collection name from the command.

          @returns the collection name
        */
        string getCollectionName() const;

        intrusive_ptr<ExpressionContext> getContext() const { return pCtx; }

        /**
          Split the current Pipeline into a Pipeline for each shard, and
          a Pipeline that combines the results within mongos.

          This permanently alters this pipeline for the merging operation.

          @returns the Spec for the pipeline command that should be sent
            to the shards
        */
        intrusive_ptr<Pipeline> splitForSharded();

        /**
           If the pipeline starts with a $match, dump its BSON predicate
           specification to the supplied builder and return true.

           @param pQueryBuilder the builder to put the match BSON into
           @returns true if a match was found and dumped to pQueryBuilder,
             false otherwise
         */
        bool getInitialQuery(BSONObjBuilder *pQueryBuilder) const;

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

        /**
          Debugging:  should the processing pipeline be split within
          mongod, simulating the real mongos/mongod split?  This is determined
          by setting the splitMongodPipeline field in an "aggregate"
          command.

          The split itself is handled by the caller, which is currently
          pipeline_command.cpp.

          @returns true if the pipeline is to be split
         */
        bool getSplitMongodPipeline() const;

        /**
           Ask if this is for an explain request.

           @returns true if this is an explain
         */
        bool isExplain() const;

        /// The initial source is special since it varies between mongos and mongod.
        void addInitialSource(intrusive_ptr<DocumentSource> source);

        /// The source that represents the output. Returns a non-owning pointer.
        DocumentSource* output() { return sources.back().get(); }

        /// Returns true if this pipeline only uses features that work in mongos.
        bool canRunInMongos() const;

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
        static const char pipelineName[];
        static const char explainName[];
        static const char fromRouterName[];
        static const char splitMongodPipelineName[];
        static const char serverPipelineName[];
        static const char mongosPipelineName[];

        Pipeline(const intrusive_ptr<ExpressionContext> &pCtx);

        /**
         * Write the pipeline's operators to a vector<Value>, with the
         * explain flag true (for DocumentSource::serializeToArray()).
         */
        vector<Value> writeExplainOps() const;

        /*
          Write the pipeline's operators to the given result document,
          for a shard server (or regular server, in an unsharded setup).

          This uses writeExplainOps() and adds that array to the result
          with the serverPipelineName.  That will be preceded by explain
          information for the input source.

          @param result the object to add the explain information to
         */
        void writeExplainShard(BSONObjBuilder &result) const;

        /*
          Write the pipeline's operators to the given result document,
          for a mongos instance.

          This first adds the serverPipeline obtained from the input
          source.

          Then this uses writeExplainOps() and adds that array to the result
          with the serverPipelineName.  That will be preceded by explain
          information for the input source.

          @param result the object to add the explain information to
         */
        void writeExplainMongos(BSONObjBuilder &result) const;

        string collectionName;
        typedef std::deque<boost::intrusive_ptr<DocumentSource> > SourceContainer;
        SourceContainer sources;
        bool explain;

        bool splitMongodPipeline;
        boost::intrusive_ptr<ExpressionContext> pCtx;
    };

} // namespace mongo


/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {

    inline string Pipeline::getCollectionName() const {
        return collectionName;
    }

    inline bool Pipeline::getSplitMongodPipeline() const {
        if (!DEBUG_BUILD)
            return false;

        return splitMongodPipeline;
    }

    inline bool  Pipeline::isExplain() const {
        return explain;
    }

} // namespace mongo


