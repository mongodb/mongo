/**
 * Copyright (c) 2011 10gen Inc.
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

#include "mongo/pch.h"

#include <boost/unordered_map.hpp>
#include <deque>

#include "mongo/db/clientcursor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/projection.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/s/shard.h"
#include "mongo/s/strategy.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {
    class Accumulator;
    class Cursor;
    class Document;
    class Expression;
    class ExpressionContext;
    class ExpressionFieldPath;
    class ExpressionObject;
    class DocumentSourceLimit;

    class DocumentSource : public IntrusiveCounterUnsigned {
    public:
        virtual ~DocumentSource();

        /**
           Set the step for a user-specified pipeline step.

           The step is used for diagnostics.

           @param step step number 0 to n.
        */
        void setPipelineStep(int step);

        /**
           Get the user-specified pipeline step.

           @returns the step number, or -1 if it has never been set
        */
        int getPipelineStep() const;

        /**
          Is the source at EOF?

          @returns true if the source has no more Documents to return.
        */
        virtual bool eof() = 0;

        /**
          Advance the state of the DocumentSource so that it will return the
          next Document.

          The default implementation returns false, after checking for
          interrupts.  Derived classes can call the default implementation
          in their own implementations in order to check for interrupts.

          @returns whether there is another document to fetch, i.e., whether or
            not getCurrent() will succeed.  This default implementation always
            returns false.
        */
        virtual bool advance();

        /** @returns the current Document without advancing.
         *
         *  It is illegal to call this without first checking eof() == false or advance() == true.
         *
         *  While it is legal to call getCurrent() multiple times between calls to advance, and
         *  you will get the same Document returned, some DocumentSources do expensive work in
         *  getCurrent(). You are advised to cache the result if you plan to access it more than
         *  once.
         */
        virtual Document getCurrent() = 0;

        /**
         * Inform the source that it is no longer needed and may release its resources.  After
         * dispose() is called the source must still be able to handle iteration requests, but may
         * become eof().
         * NOTE: For proper mutex yielding, dispose() must be called on any DocumentSource that will
         * not be advanced until eof(), see SERVER-6123.
         */
        virtual void dispose();

        /**
           Get the source's name.

           @returns the string name of the source as a constant string;
             this is static, and there's no need to worry about adopting it
         */
        virtual const char *getSourceName() const;

        /**
          Set the underlying source this source should use to get Documents
          from.

          It is an error to set the source more than once.  This is to
          prevent changing sources once the original source has been started;
          this could break the state maintained by the DocumentSource.

          This pointer is not reference counted because that has led to
          some circular references.  As a result, this doesn't keep
          sources alive, and is only intended to be used temporarily for
          the lifetime of a Pipeline::run().

          @param pSource the underlying source to use
         */
        virtual void setSource(DocumentSource *pSource);

        /**
          Attempt to coalesce this DocumentSource with its successor in the
          document processing pipeline.  If successful, the successor
          DocumentSource should be removed from the pipeline and discarded.

          If successful, this operation can be applied repeatedly, in an
          attempt to coalesce several sources together.

          The default implementation is to do nothing, and return false.

          @param pNextSource the next source in the document processing chain.
          @returns whether or not the attempt to coalesce was successful or not;
            if the attempt was not successful, nothing has been changed
         */
        virtual bool coalesce(const intrusive_ptr<DocumentSource> &pNextSource);

        /**
          Optimize the pipeline operation, if possible.  This is a local
          optimization that only looks within this DocumentSource.  For best
          results, first coalesce compatible sources using coalesce().

          This is intended for any operations that include expressions, and
          provides a hook for those to optimize those operations.

          The default implementation is to do nothing.
         */
        virtual void optimize();

        enum GetDepsReturn {
            NOT_SUPPORTED, // This means the set should be ignored
            EXHAUSTIVE, // This means that everything needed should be in the set
            SEE_NEXT, // Add the next Source's deps to the set
        };

        /** Get the fields this operation needs to do its job.
         *  Deps should be in "a.b.c" notation
         *
         *  @param deps results are added here. NOT CLEARED
         */
        virtual GetDepsReturn getDependencies(set<string>& deps) const {
            return NOT_SUPPORTED;
        }

        /** This takes dependencies from getDependencies and
         *  returns a projection that includes all of them
         */
        static BSONObj depsToProjection(const set<string>& deps);

        /** These functions take the same input as depsToProjection but are able to
         *  produce a Document from a BSONObj with the needed fields much faster.
         */
        typedef Document ParsedDeps; // See implementation for structure
        static ParsedDeps parseDeps(const set<string>& deps);
        static Document documentFromBsonWithDeps(const BSONObj& object, const ParsedDeps& deps);

        /**
          Add the DocumentSource to the array builder.

          The default implementation calls sourceToBson() in order to
          convert the inner part of the object which will be added to the
          array being built here.

          A subclass may choose to overwrite this rather than addToBsonArray
          if it should output multiple stages.

          @param pBuilder the array builder to add the operation to.
          @param explain create explain output
         */
        virtual void addToBsonArray(BSONArrayBuilder *pBuilder, bool explain=false) const;
        
    protected:
        /**
           Base constructor.
         */
        DocumentSource(const intrusive_ptr<ExpressionContext> &pExpCtx);

        /**
          Create an object that represents the document source.  The object
          will have a single field whose name is the source's name.  This
          will be used by the default implementation of addToBsonArray()
          to add this object to a pipeline being represented in BSON.

          @param pBuilder a blank object builder to write to
          @param explain create explain output
         */
        virtual void sourceToBson(BSONObjBuilder *pBuilder,
                                  bool explain) const = 0;

        /*
          Most DocumentSources have an underlying source they get their data
          from.  This is a convenience for them.

          The default implementation of setSource() sets this; if you don't
          need a source, override that to verify().  The default is to
          verify() if this has already been set.
        */
        DocumentSource *pSource;

        /*
          The zero-based user-specified pipeline step.  Used for diagnostics.
          Will be set to -1 for artificial pipeline steps that were not part
          of the original user specification.
         */
        int step;

        intrusive_ptr<ExpressionContext> pExpCtx;

        /*
          for explain: # of rows returned by this source

          This is *not* unsigned so it can be passed to BSONObjBuilder.append().
         */
        long long nRowsOut;
    };

    /** This class marks DocumentSources that should be split between the router and the shards
     *  See Pipeline::splitForSharded() for details
     */
    class SplittableDocumentSource : public DocumentSource {
    public:
        /** returns a source to be run on the shards.
         *  if NULL, don't run on shards
         */
        virtual intrusive_ptr<DocumentSource> getShardSource() = 0;

        /** returns a source that combines results from shards.
         *  if NULL, don't run on router
         */
        virtual intrusive_ptr<DocumentSource> getRouterSource() = 0;
    protected:
        SplittableDocumentSource(intrusive_ptr<ExpressionContext> ctx) :DocumentSource(ctx) {}
    };


    class DocumentSourceBsonArray :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceBsonArray();
        virtual bool eof();
        virtual bool advance();
        virtual Document getCurrent();
        virtual void setSource(DocumentSource *pSource);

        /**
          Create a document source based on a BSON array.

          This is usually put at the beginning of a chain of document sources
          in order to fetch data from the database.

          CAUTION:  the BSON is not read until the source is used.  Any
          elements that appear after these documents must not be read until
          this source is exhausted.

          @param pBsonElement the BSON array to treat as a document source
          @param pExpCtx the expression context for the pipeline
          @returns the newly created document source
        */
        static intrusive_ptr<DocumentSourceBsonArray> create(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

    private:
        DocumentSourceBsonArray(BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        BSONObj embeddedObject;
        BSONObjIterator arrayIterator;
        BSONElement currentElement;
        bool haveCurrent;
    };

    
    class DocumentSourceCommandShards :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceCommandShards();
        virtual bool eof();
        virtual bool advance();
        virtual Document getCurrent();
        virtual void setSource(DocumentSource *pSource);

        /* convenient shorthand for a commonly used type */
        typedef vector<Strategy::CommandResult> ShardOutput;

        /**
          Create a DocumentSource that wraps the output of many shards

          @param shardOutput output from the individual shards
          @param pExpCtx the expression context for the pipeline
          @returns the newly created DocumentSource
         */
        static intrusive_ptr<DocumentSourceCommandShards> create(
            const ShardOutput& shardOutput,
            const intrusive_ptr<ExpressionContext>& pExpCtx);

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

    private:
        DocumentSourceCommandShards(const ShardOutput& shardOutput,
            const intrusive_ptr<ExpressionContext>& pExpCtx);

        /**
          Advance to the next document, setting pCurrent appropriately.

          Adjusts pCurrent, pBsonSource, and iterator, as needed.  On exit,
          pCurrent is the Document to return, or NULL.  If NULL, this
          indicates there is nothing more to return.
         */
        void getNextDocument();

        bool unstarted;
        bool hasCurrent;
        bool newSource; // set to true for the first item of a new source
        intrusive_ptr<DocumentSourceBsonArray> pBsonSource;
        Document pCurrent;
        ShardOutput::const_iterator iterator;
        ShardOutput::const_iterator listEnd;
    };


    /**
     * Constructs and returns Documents from the BSONObj objects produced by a supplied Cursor.
     * An object of this type may only be used by one thread, see SERVER-6123.
     */
    class DocumentSourceCursor :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceCursor();
        virtual bool eof();
        virtual bool advance();
        virtual Document getCurrent();
        virtual void setSource(DocumentSource *pSource);
        virtual bool coalesce(const intrusive_ptr<DocumentSource>& nextSource);

        /**
         * Release the Cursor and the read lock it requires, but without changing the other data.
         * Releasing the lock is required for proper concurrency, see SERVER-6123.  This
         * functionality is also used by the explain version of pipeline execution.
         */
        virtual void dispose();

        /**
         * Create a document source based on a passed-in cursor.
         *
         * This is usually put at the beginning of a chain of document sources
         * in order to fetch data from the database.
         *
         * The DocumentSource takes ownership of the cursor and will destroy it
         * when the DocumentSource is finished with the cursor, if it hasn't
         * already been destroyed.
         *
         * @param ns the namespace the cursor is over
         * @param cursorId the id of the cursor to use
         * @param pExpCtx the expression context for the pipeline
         */
        static intrusive_ptr<DocumentSourceCursor> create(
            const string& ns,
            CursorId cursorId,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        /*
          Record the namespace.  Required for explain.

          @param namespace the namespace
        */

        /*
          Record the query that was specified for the cursor this wraps, if
          any.

          This should be captured after any optimizations are applied to
          the pipeline so that it reflects what is really used.

          This gets used for explain output.

          @param pBsonObj the query to record
         */
        void setQuery(const BSONObj& query) { _query = query; }

        /*
          Record the sort that was specified for the cursor this wraps, if
          any.

          This should be captured after any optimizations are applied to
          the pipeline so that it reflects what is really used.

          This gets used for explain output.

          @param pBsonObj the sort to record
         */
        void setSort(const BSONObj& sort) { _sort = sort; }

        void setProjection(const BSONObj& projection, const ParsedDeps& deps);

        /// returns -1 for no limit
        long long getLimit() const;
    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

    private:
        DocumentSourceCursor(
            const string& ns,
            CursorId cursorId,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        void loadBatch();

        bool unstarted;
        std::deque<Document> _currentBatch;

        // BSONObj members must outlive _projection and cursor.
        BSONObj _query;
        BSONObj _sort;
        shared_ptr<Projection> _projection; // shared with pClientCursor
        ParsedDeps _dependencies;
        intrusive_ptr<DocumentSourceLimit> _limit;
        long long _docsAddedToBatches; // for _limit enforcement

        string ns; // namespace
        CursorId _cursorId;
        CollectionMetadataPtr _collMetadata;

        bool canUseCoveredIndex(ClientCursor* cursor);

        /*
          Yield the cursor sometimes.

          If the state of the world changed during the yield such that we
          are unable to continue execution of the query, this will release the
          client cursor, and throw an error.  NOTE This differs from the
          behavior of most other operations, see SERVER-2454.
         */
        void yieldSometimes(ClientCursor* cursor);
    };


    /*
      This contains all the basic mechanics for filtering a stream of
      Documents, except for the actual predicate evaluation itself.  This was
      factored out so we could create DocumentSources that use both Matcher
      style predicates as well as full Expressions.
     */
    class DocumentSourceFilterBase :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceFilterBase();
        virtual bool eof();
        virtual bool advance();
        virtual Document getCurrent();

        /**
          Create a BSONObj suitable for Matcher construction.

          This is used after filter analysis has moved as many filters to
          as early a point as possible in the document processing pipeline.
          See db/Matcher.h and the associated documentation for the format.
          This conversion is used to move back to the low-level find()
          Cursor mechanism.

          @param pBuilder the builder to write to
         */
        virtual void toMatcherBson(BSONObjBuilder *pBuilder) const = 0;

    protected:
        DocumentSourceFilterBase(
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        /**
          Test the given document against the predicate and report if it
          should be accepted or not.

          @param pDocument the document to test
          @returns true if the document matches the filter, false otherwise
         */
        virtual bool accept(const Document& pDocument) const = 0;

    private:

        void findNext();

        bool unstarted;
        bool hasCurrent;
        Document pCurrent;
    };


    class DocumentSourceFilter :
        public DocumentSourceFilterBase {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceFilter();
        virtual bool coalesce(const intrusive_ptr<DocumentSource> &pNextSource);
        virtual void optimize();
        virtual const char *getSourceName() const;

        /**
          Create a filter.

          @param pBsonElement the raw BSON specification for the filter
          @param pExpCtx the expression context for the pipeline
          @returns the filter
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        /**
          Create a filter.

          @param pFilter the expression to use to filter
          @param pExpCtx the expression context for the pipeline
          @returns the filter
         */
        static intrusive_ptr<DocumentSourceFilter> create(
            const intrusive_ptr<Expression> &pFilter,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        /**
          Create a BSONObj suitable for Matcher construction.

          This is used after filter analysis has moved as many filters to
          as early a point as possible in the document processing pipeline.
          See db/Matcher.h and the associated documentation for the format.
          This conversion is used to move back to the low-level find()
          Cursor mechanism.

          @param pBuilder the builder to write to
         */
        void toMatcherBson(BSONObjBuilder *pBuilder) const;

        static const char filterName[];

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

        // virtuals from DocumentSourceFilterBase
        virtual bool accept(const Document& pDocument) const;

    private:
        DocumentSourceFilter(const intrusive_ptr<Expression> &pFilter,
                             const intrusive_ptr<ExpressionContext> &pExpCtx);

        intrusive_ptr<Expression> pFilter;
    };


    class DocumentSourceGroup :
        public SplittableDocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceGroup();
        virtual bool eof();
        virtual bool advance();
        virtual const char *getSourceName() const;
        virtual Document getCurrent();
        virtual GetDepsReturn getDependencies(set<string>& deps) const;
        virtual void dispose();

        /**
          Create a new grouping DocumentSource.
          
          @param pExpCtx the expression context for the pipeline
          @returns the DocumentSource
         */
        static intrusive_ptr<DocumentSourceGroup> create(
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        /**
          Set the Id Expression.

          Documents that pass through the grouping Document are grouped
          according to this key.  This will generate the id_ field in the
          result documents.

          @param pExpression the group key
         */
        void setIdExpression(const intrusive_ptr<Expression> &pExpression);

        /**
          Add an accumulator.

          Accumulators become fields in the Documents that result from
          grouping.  Each unique group document must have it's own
          accumulator; the accumulator factory is used to create that.

          @param fieldName the name the accumulator result will have in the
                result documents
          @param pAccumulatorFactory used to create the accumulator for the
                group field
         */
        void addAccumulator(const std::string& fieldName,
                            intrusive_ptr<Accumulator> (*pAccumulatorFactory)(),
                            const intrusive_ptr<Expression> &pExpression);

        /**
          Create a grouping DocumentSource from BSON.

          This is a convenience method that uses the above, and operates on
          a BSONElement that has been deteremined to be an Object with an
          element named $group.

          @param pBsonElement the BSONELement that defines the group
          @param pExpCtx the expression context
          @returns the grouping DocumentSource
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        // Virtuals for SplittableDocumentSource
        virtual intrusive_ptr<DocumentSource> getShardSource();
        virtual intrusive_ptr<DocumentSource> getRouterSource();

        static const char groupName[];

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

    private:
        DocumentSourceGroup(const intrusive_ptr<ExpressionContext> &pExpCtx);

        /// Spill groups map to disk and returns an iterator to the file.
        shared_ptr<Sorter<Value, Value>::Iterator> spill();

        // Only used by spill. Would be function-local if that were legal in C++03.
        class SpillSTLComparator;

        /*
          Before returning anything, this source must fetch everything from
          the underlying source and group it.  populate() is used to do that
          on the first call to any method on this source.  The populated
          boolean indicates that this has been done.
         */
        void populate();
        bool populated;

        intrusive_ptr<Expression> pIdExpression;

        typedef vector<intrusive_ptr<Accumulator> > Accumulators;
        typedef boost::unordered_map<Value, Accumulators, Value::Hash> GroupsMap;
        GroupsMap groups;

        /*
          The field names for the result documents and the accumulator
          factories for the result documents.  The Expressions are the
          common expressions used by each instance of each accumulator
          in order to find the right-hand side of what gets added to the
          accumulator.  Note that each of those is the same for each group,
          so we can share them across all groups by adding them to the
          accumulators after we use the factories to make a new set of
          accumulators for each new group.

          These three vectors parallel each other.
        */
        vector<string> vFieldName;
        vector<intrusive_ptr<Accumulator> (*)()> vpAccumulatorFactory;
        vector<intrusive_ptr<Expression> > vpExpression;


        Document makeDocument(const Value& id, const Accumulators& accums, bool mergeableOutput);

        bool _spilled;
        const bool _extSortAllowed;
        const int _maxMemoryUsageBytes;

        // only used when !_spilled
        GroupsMap::iterator groupsIterator;

        // only used when _spilled
        scoped_ptr<Sorter<Value, Value>::Iterator> _sorterIterator;
        pair<Value, Value> _firstPartOfNextGroup;
        Value _currentId;
        Accumulators _currentAccumulators;
        bool _doneAfterNextAdvance;
        bool _done;
    };


    class DocumentSourceMatch :
        public DocumentSourceFilterBase {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceMatch();
        virtual const char *getSourceName() const;

        /**
          Create a filter.

          @param pBsonElement the raw BSON specification for the filter
          @returns the filter
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pCtx);

        /**
          Create a BSONObj suitable for Matcher construction.

          This is used after filter analysis has moved as many filters to
          as early a point as possible in the document processing pipeline.
          See db/Matcher.h and the associated documentation for the format.
          This conversion is used to move back to the low-level find()
          Cursor mechanism.

          @param pBuilder the builder to write to
         */
        void toMatcherBson(BSONObjBuilder *pBuilder) const;

        static const char matchName[];

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

        // virtuals from DocumentSourceFilterBase
        virtual bool accept(const Document& pDocument) const;

    private:
        DocumentSourceMatch(const BSONObj &query,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        Matcher matcher;
    };


    class DocumentSourceOut :
        public SplittableDocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceOut();
        virtual bool eof();
        virtual bool advance();
        virtual const char *getSourceName() const;
        virtual Document getCurrent();

        // Virtuals for SplittableDocumentSource
        virtual intrusive_ptr<DocumentSource> getShardSource() { return NULL; }
        virtual intrusive_ptr<DocumentSource> getRouterSource() { return this; }

        /// Gives $out a way to add the outputNs to the command result.
        void alterCommandResult(BSONObjBuilder& cmdResult);

        /**
          Create a document source for output and pass-through.

          This can be put anywhere in a pipeline and will store content as
          well as pass it on.

          @param pBsonElement the raw BSON specification for the source
          @param pExpCtx the expression context for the pipeline
          @returns the newly created document source
        */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        static const char outName[];

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

    private:
        DocumentSourceOut(const NamespaceString& outputNs,
                          const intrusive_ptr<ExpressionContext> &pExpCtx);

        // Sets _tempsNs and prepares it to receive data.
        void prepTempCollection();

        bool _done;

        NamespaceString _tempNs; // output goes here as it is being processed.
        const NamespaceString _outputNs; // output will go here after all data is processed.

        // This field is injected by PipelineD. This division of labor allows the
        // DocumentSourceOut class to be linked into both mongos and mongod while
        // allowing it to use DBDirectClient when in mongod.
        boost::scoped_ptr<DBClientBase> _conn; // either NULL or a DBDirectClient
        friend class PipelineD;
    };

    
    class DocumentSourceProject :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceProject();
        virtual bool eof();
        virtual bool advance();
        virtual const char *getSourceName() const;
        virtual Document getCurrent();
        virtual void optimize();

        virtual GetDepsReturn getDependencies(set<string>& deps) const;

        /**
          Create a new projection DocumentSource from BSON.

          This is a convenience for directly handling BSON, and relies on the
          above methods.

          @param pBsonElement the BSONElement with an object named $project
          @param pExpCtx the expression context for the pipeline
          @returns the created projection
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        static const char projectName[];

        /** projection as specified by the user */
        BSONObj getRaw() const { return _raw; }

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

    private:
        DocumentSourceProject(const intrusive_ptr<ExpressionContext>& pExpCtx,
                              const intrusive_ptr<ExpressionObject>& exprObj);

        // configuration state
        intrusive_ptr<ExpressionObject> pEO;
        BSONObj _raw;

#if defined(_DEBUG)
        // this is used in DEBUG builds to ensure we are compatible
        Projection _simpleProjection;
#endif
    };


    class DocumentSourceSort :
        public SplittableDocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceSort();
        virtual bool eof();
        virtual bool advance();
        virtual const char *getSourceName() const;
        virtual Document getCurrent();
        virtual void addToBsonArray(BSONArrayBuilder *pBuilder, bool explain=false) const;
        virtual bool coalesce(const intrusive_ptr<DocumentSource> &pNextSource);
        virtual void dispose();

        virtual GetDepsReturn getDependencies(set<string>& deps) const;

        // Virtuals for SplittableDocumentSource
        // All work for sort is done in router currently if there is no limit.
        // If there is a limit, the $sort/$limit combination is performed on the
        // shards, then the results are resorted and limited on mongos
        virtual intrusive_ptr<DocumentSource> getShardSource() { return limitSrc ? this : NULL; }
        virtual intrusive_ptr<DocumentSource> getRouterSource() { return this; }

        /**
          Add sort key field.

          Adds a sort key field to the key being built up.  A concatenated
          key is built up by calling this repeatedly.

          @param fieldPath the field path to the key component
          @param ascending if true, use the key for an ascending sort,
            otherwise, use it for descending
        */
        void addKey(const string &fieldPath, bool ascending);

        /**
          Write out an object whose contents are the sort key.

          @param pBuilder initialized object builder.
          @param fieldPrefix specify whether or not to include the field prefix
         */
        void sortKeyToBson(BSONObjBuilder *pBuilder, bool usePrefix) const;

        /**
          Create a sorting DocumentSource from BSON.

          This is a convenience method that uses the above, and operates on
          a BSONElement that has been deteremined to be an Object with an
          element named $group.

          @param pBsonElement the BSONELement that defines the group
          @param pExpCtx the expression context for the pipeline
          @returns the grouping DocumentSource
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        /// Create a DocumentSourceSort with a given sort and (optional) limit
        static intrusive_ptr<DocumentSourceSort> create(
            const intrusive_ptr<ExpressionContext> &pExpCtx,
            BSONObj sortOrder,
            long long limit=-1);

        /// returns -1 for no limit
        long long getLimit() const;

        intrusive_ptr<DocumentSourceLimit> getLimitSrc() const { return limitSrc; }

        static const char sortName[];
    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const {
            verify(false); // should call addToBsonArray instead
        }

    private:
        DocumentSourceSort(const intrusive_ptr<ExpressionContext> &pExpCtx);

        /*
          Before returning anything, this source must fetch everything from
          the underlying source and group it.  populate() is used to do that
          on the first call to any method on this source.  The populated
          boolean indicates that this has been done.
         */
        void populate();
        bool populated;

        /* these two parallel each other */
        typedef vector<intrusive_ptr<ExpressionFieldPath> > SortPaths;
        SortPaths vSortKey;
        vector<char> vAscending; // used like vector<bool> but without specialization

        /// Extracts the fields in vSortKey from the Document;
        Value extractKey(const Document& d) const;

        /// Compare two Values according to the specified sort key.
        int compare(const Value& lhs, const Value& rhs) const;

        typedef Sorter<Value, Document> MySorter;

        // For MySorter
        class Comparator {
        public:
            explicit Comparator(const DocumentSourceSort& source): _source(source) {}
            int operator()(const MySorter::Data& lhs, const MySorter::Data& rhs) const {
                return _source.compare(lhs.first, rhs.first);
            }
        private:
            const DocumentSourceSort& _source;
        };

        intrusive_ptr<DocumentSourceLimit> limitSrc;

        bool _done;
        Document _current;
        scoped_ptr<MySorter::Iterator> _output;
    };

    class DocumentSourceLimit :
        public SplittableDocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceLimit();
        virtual bool eof();
        virtual bool advance();
        virtual Document getCurrent();
        virtual const char *getSourceName() const;
        virtual bool coalesce(const intrusive_ptr<DocumentSource> &pNextSource);

        virtual GetDepsReturn getDependencies(set<string>& deps) const {
            return SEE_NEXT; // This doesn't affect needed fields
        }

        /**
          Create a new limiting DocumentSource.

          @param pExpCtx the expression context for the pipeline
          @returns the DocumentSource
         */
        static intrusive_ptr<DocumentSourceLimit> create(
            const intrusive_ptr<ExpressionContext> &pExpCtx,
            long long limit);

        // Virtuals for SplittableDocumentSource
        // Need to run on rounter. Running on shard as well is an optimization.
        virtual intrusive_ptr<DocumentSource> getShardSource() { return this; }
        virtual intrusive_ptr<DocumentSource> getRouterSource() { return this; }

        long long getLimit() const { return limit; }
        void setLimit(long long newLimit) { limit = newLimit; }

        /**
          Create a limiting DocumentSource from BSON.

          This is a convenience method that uses the above, and operates on
          a BSONElement that has been deteremined to be an Object with an
          element named $limit.

          @param pBsonElement the BSONELement that defines the limit
          @param pExpCtx the expression context
          @returns the grouping DocumentSource
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        static const char limitName[];

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

    private:
        DocumentSourceLimit(const intrusive_ptr<ExpressionContext> &pExpCtx,
                            long long limit);

        long long limit;
        long long count;
    };

    class DocumentSourceSkip :
        public SplittableDocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceSkip();
        virtual bool eof();
        virtual bool advance();
        virtual Document getCurrent();
        virtual const char *getSourceName() const;
        virtual bool coalesce(const intrusive_ptr<DocumentSource> &pNextSource);

        virtual GetDepsReturn getDependencies(set<string>& deps) const {
            return SEE_NEXT; // This doesn't affect needed fields
        }

        /**
          Create a new skipping DocumentSource.

          @param pExpCtx the expression context
          @returns the DocumentSource
         */
        static intrusive_ptr<DocumentSourceSkip> create(
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        // Virtuals for SplittableDocumentSource
        // Need to run on rounter. Can't run on shards.
        virtual intrusive_ptr<DocumentSource> getShardSource() { return NULL; }
        virtual intrusive_ptr<DocumentSource> getRouterSource() { return this; }

        long long getSkip() const { return skip; }
        void setSkip(long long newSkip) { skip = newSkip; }

        /**
          Create a skipping DocumentSource from BSON.

          This is a convenience method that uses the above, and operates on
          a BSONElement that has been deteremined to be an Object with an
          element named $skip.

          @param pBsonElement the BSONELement that defines the skip
          @param pExpCtx the expression context
          @returns the grouping DocumentSource
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        static const char skipName[];

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

    private:
        DocumentSourceSkip(const intrusive_ptr<ExpressionContext> &pExpCtx);

        /*
          Skips initial documents.
         */
        void skipper();

        long long skip;
        long long count;
        Document pCurrent;
    };


    class DocumentSourceUnwind :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceUnwind();
        virtual bool eof();
        virtual bool advance();
        virtual const char *getSourceName() const;
        virtual Document getCurrent();

        virtual GetDepsReturn getDependencies(set<string>& deps) const;

        /**
          Create a new projection DocumentSource from BSON.

          This is a convenience for directly handling BSON, and relies on the
          above methods.

          @param pBsonElement the BSONElement with an object named $project
          @param pExpCtx the expression context for the pipeline
          @returns the created projection
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        static const char unwindName[];

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

    private:
        DocumentSourceUnwind(const intrusive_ptr<ExpressionContext> &pExpCtx);

        /**
         * Lazily construct the _unwinder and initialize the iterator state of this DocumentSource.
         * To be called by all members that depend on the iterator state.
         */
        void lazyInit();

        /**
         * If the _unwinder is exhausted and the source may be advanced, advance the pSource and
         * reset the _unwinder's source document.
         */
        void mayAdvanceSource();

        /** Specify the field to unwind. */
        void unwindPath(const FieldPath &fieldPath);

        // Configuration state.
        scoped_ptr<FieldPath> _unwindPath;

        // Iteration state.
        class Unwinder;
        scoped_ptr<Unwinder> _unwinder;
    };

    class DocumentSourceGeoNear : public SplittableDocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceGeoNear();
        virtual bool eof();
        virtual bool advance();
        virtual Document getCurrent();
        virtual const char *getSourceName() const;
        virtual void setSource(DocumentSource *pSource); // errors out since this must be first
        virtual bool coalesce(const intrusive_ptr<DocumentSource> &pNextSource);

        // Virtuals for SplittableDocumentSource
        virtual intrusive_ptr<DocumentSource> getShardSource();
        virtual intrusive_ptr<DocumentSource> getRouterSource();

        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pCtx);

        static char geoNearName[];

        long long getLimit() { return limit; }

        // this should only be used for testing
        static intrusive_ptr<DocumentSourceGeoNear> create(
            const intrusive_ptr<ExpressionContext> &pCtx);

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

    private:
        DocumentSourceGeoNear(const intrusive_ptr<ExpressionContext> &pExpCtx);

        void parseOptions(BSONObj options);
        BSONObj buildGeoNearCmd() const;
        void runCommand();

        // These fields describe the command to run.
        // coords and distanceField are required, rest are optional
        BSONObj coords; // "near" option, but near is a reserved keyword on windows
        bool coordsIsArray;
        scoped_ptr<FieldPath> distanceField; // Using scoped_ptr because FieldPath can't be empty
        long long limit;
        double maxDistance;
        BSONObj query;
        bool spherical;
        double distanceMultiplier;
        scoped_ptr<FieldPath> includeLocs;
        bool uniqueDocs;

        // This field is injected by PipelineD. This division of labor allows the
        // DocumentSourceGeoNear class to be linked into both mongos and mongod while
        // allowing it to run a command using DBDirectClient when in mongod.
        boost::scoped_ptr<DBClientWithCommands> client; // either NULL or a DBDirectClient
        friend class PipelineD;

        // these fields are used while processing the results
        BSONObj cmdOutput;
        boost::scoped_ptr<BSONObjIterator> resultsIterator; // iterator over cmdOutput["results"]
        Document currentDoc;
        bool hasCurrent;
    };
}


/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {

    inline void DocumentSource::setPipelineStep(int s) {
        step = s;
    }

    inline int DocumentSource::getPipelineStep() const {
        return step;
    }
    
    inline void DocumentSourceGroup::setIdExpression(
        const intrusive_ptr<Expression> &pExpression) {
        pIdExpression = pExpression;
    }
}
