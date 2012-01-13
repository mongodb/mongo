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
 */

#pragma once

#include "pch.h"

#include "db/commands.h"
#include "db/jsobj.h"
#include "util/intrusive_counter.h"
#include "util/timer.h"

namespace mongo {
    class BSONObj;
    class BSONObjBuilder;
    class DocumentSource;
    class DocumentSourceProject;
    class Expression;
    class ExpressionContext;
    class ExpressionNary;
    struct OpDesc; // local private struct

    /** mongodb "commands" (sent via db.$cmd.findOne(...))
        subclass to make a command.  define a singleton object for it.
        */
    class Pipeline :
        public IntrusiveCounterUnsigned {
    public:
        virtual ~Pipeline();

        /**
          Create a pipeline from the command.

          @param errmsg where to write errors, if there are any
          @param cmdObj the command object sent from the client
          @returns the pipeline, if created, otherwise a NULL reference
         */
        static intrusive_ptr<Pipeline> parseCommand(
            string &errmsg, BSONObj &cmdObj,
            const intrusive_ptr<ExpressionContext> &pCtx);

        /**
          Get the collection name from the command.

          @returns the collection name
        */
        string getCollectionName() const;

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
        void toBson(BSONObjBuilder *pBuilder) const;

        /**
          Run the Pipeline on the given source.

          @param result builder to write the result to
          @param errmsg place to put error messages, if any
          @param pSource the document source to use at the head of the chain
          @returns true on success, false if an error occurs
        */
        bool run(BSONObjBuilder &result, string &errmsg,
                 const intrusive_ptr<DocumentSource> &pSource);

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
        static const char fromRouterName[];
        static const char splitMongodPipelineName[];

        Pipeline(const intrusive_ptr<ExpressionContext> &pCtx);

        string collectionName;
        typedef vector<intrusive_ptr<DocumentSource> > SourceVector;
        SourceVector sourceVector;

        bool splitMongodPipeline;
        intrusive_ptr<ExpressionContext> pCtx;
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

} // namespace mongo


