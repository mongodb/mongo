// query.h

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "../../pch.h"
#include "../../util/net/message.h"
#include "../dbmessage.h"
#include "../jsobj.h"
#include "../diskloc.h"
#include "../explain.h"
#include "../queryoptimizer.h"
#include "../queryoptimizercursor.h"
#include "../../s/d_chunk_manager.h"

// struct QueryOptions, QueryResult, QueryResultFlags in:
#include "../../client/dbclient.h"

namespace mongo {

    QueryResult* processGetMore(const char *ns, int ntoreturn, long long cursorid , CurOp& op, int pass, bool& exhaust);

    const char * runQuery(Message& m, QueryMessage& q, CurOp& curop, Message &result);

    /** Exception indicating that a query should be retried from the beginning. */
    class QueryRetryException : public DBException {
    public:
        QueryRetryException() : DBException( "query retry exception" , 16083 ) {
            return;
            verify( 16083, true ); // Reserve 16083.
        }
    };

    /** Interface for recording events that contribute to explain results. */
    class ExplainRecordingStrategy {
    public:
        ExplainRecordingStrategy( const ExplainQueryInfo::AncillaryInfo &ancillaryInfo );
        virtual ~ExplainRecordingStrategy() {}
        /** Note information about a single query plan. */
        virtual void notePlan( bool scanAndOrder, bool indexOnly ) {}
        /** Note an iteration of the query. */
        virtual void noteIterate( bool match, bool loadedObject, bool chunkSkip ) {}
        /** Note that the query yielded. */
        virtual void noteYield() {}
        /** @return ExplainQueryInfo for a complete query. */
        shared_ptr<ExplainQueryInfo> doneQueryInfo();
    protected:
        /** @return ExplainQueryInfo for a complete query, to be implemented by subclass. */
        virtual shared_ptr<ExplainQueryInfo> _doneQueryInfo() = 0;
    private:
        ExplainQueryInfo::AncillaryInfo _ancillaryInfo;
    };
    
    /** No explain events are recorded. */
    class NoExplainStrategy : public ExplainRecordingStrategy {
    public:
        NoExplainStrategy();
    private:
        /** @asserts always. */
        virtual shared_ptr<ExplainQueryInfo> _doneQueryInfo();
    };
    
    /** Record explain events for a simple cursor representing a single clause and plan. */
    class SimpleCursorExplainStrategy : public ExplainRecordingStrategy {
    public:
        SimpleCursorExplainStrategy( const ExplainQueryInfo::AncillaryInfo &ancillaryInfo,
                                    const shared_ptr<Cursor> &cursor );
    private:
        virtual void notePlan( bool scanAndOrder, bool indexOnly );
        virtual void noteIterate( bool match, bool loadedObject, bool chunkSkip );
        virtual void noteYield();
        virtual shared_ptr<ExplainQueryInfo> _doneQueryInfo();
        shared_ptr<Cursor> _cursor;
        shared_ptr<ExplainSinglePlanQueryInfo> _explainInfo;
    };
    
    /**
     * Record explain events for a QueryOptimizerCursor, which may record some explain information
     * for multiple clauses and plans through an internal implementation.
     */
    class QueryOptimizerCursorExplainStrategy : public ExplainRecordingStrategy {
    public:
        QueryOptimizerCursorExplainStrategy( const ExplainQueryInfo::AncillaryInfo &ancillaryInfo,
                                            const shared_ptr<QueryOptimizerCursor> &cursor );
    private:
        virtual void noteIterate( bool match, bool loadedObject, bool chunkSkip );
        virtual shared_ptr<ExplainQueryInfo> _doneQueryInfo();
        shared_ptr<QueryOptimizerCursor> _cursor;
    };

    /** Interface for building a query response in a supplied BufBuilder. */
    class ResponseBuildStrategy {
    public:
        /**
         * @param queryPlan must be supplied if @param cursor is not a QueryOptimizerCursor and
         * results must be sorted or read with a covered index.
         */
        ResponseBuildStrategy( const ParsedQuery &parsedQuery, const shared_ptr<Cursor> &cursor,
                              BufBuilder &buf, const QueryPlan::Summary &queryPlan );
        virtual ~ResponseBuildStrategy() {}
        /**
         * Handle the current iterate of the supplied cursor as a (possibly duplicate) match.
         * @return true if the match is added to the buffer (or would be if not an explain query).
         * (If a match is saved for later use but not added to the buffer, @return false.)
         */
        virtual bool handleMatch() = 0;
        /**
         * Write all matches into the buffer, overwriting existing data.
         * @return number of matches written, or -1 if no op.
         */
        virtual int rewriteMatches() { return -1; }
        /**
         * Callback when enough results have been read for the first batch, with potential handoff
         * to getMore.
         */
        virtual void finishedFirstBatch() {}
        /** Reset the buffer. */
        void resetBuf();
    protected:
        /**
         * Return the document for the current iterate.  Implements the $returnKey option.
         * @param allowCovered - enable covered index support.
         */
        BSONObj current( bool allowCovered ) const;
        const ParsedQuery &_parsedQuery;
        shared_ptr<Cursor> _cursor;
        shared_ptr<QueryOptimizerCursor> _queryOptimizerCursor;
        BufBuilder &_buf;
    private:
        const Projection::KeyOnly *keyFieldsOnly() const;
        shared_ptr<Projection::KeyOnly> _planKeyFieldsOnly;
    };

    /** Build strategy for a cursor returning in order results. */
    class OrderedBuildStrategy : public ResponseBuildStrategy {
    public:
        OrderedBuildStrategy( const ParsedQuery &parsedQuery, const shared_ptr<Cursor> &cursor,
                             BufBuilder &buf, const QueryPlan::Summary &queryPlan );
        virtual bool handleMatch();
    private:
        long long _skip;
    };
    
    class ScanAndOrder;
    
    /** Build strategy for a cursor returning out of order results. */
    class ReorderBuildStrategy : public ResponseBuildStrategy {
    public:
        ReorderBuildStrategy( const ParsedQuery &parsedQuery,
                             const shared_ptr<Cursor> &cursor,
                             BufBuilder &buf,
                             const QueryPlan::Summary &queryPlan );
        virtual bool handleMatch();
        /** Handle a match without performing deduping. */
        bool _handleMatchNoDedup();
        virtual int rewriteMatches();
    private:
        ScanAndOrder *newScanAndOrder( const QueryPlan::Summary &queryPlan ) const;
        shared_ptr<ScanAndOrder> _scanAndOrder;
    };

    /**
     * Build strategy for a QueryOptimizerCursor containing some in order and some out of order
     * candidate plans.
     */
    class HybridBuildStrategy : public ResponseBuildStrategy {
    public:
        HybridBuildStrategy( const ParsedQuery &parsedQuery,
                            const shared_ptr<QueryOptimizerCursor> &cursor,
                            BufBuilder &buf );
    private:
        virtual bool handleMatch();
        virtual int rewriteMatches();
        virtual void finishedFirstBatch();
        void handleReorderMatch();
        bool handleOrderedMatch();
        DiskLocDupSet _scanAndOrderDups;
        OrderedBuildStrategy _orderedBuild;
        ReorderBuildStrategy _reorderBuild;
    };

    /**
     * Builds a query response with the help of an ExplainRecordingStrategy and a
     * ResponseBuildStrategy.
     */
    class QueryResponseBuilder {
    public:
        /**
         * @param queryPlan must be supplied if @param cursor is not a QueryOptimizerCursor and
         * results must be sorted or read with a covered index.
         */
        QueryResponseBuilder( const ParsedQuery &parsedQuery, const shared_ptr<Cursor> &cursor,
                             const QueryPlan::Summary &queryPlan, const BSONObj &oldPlan );
        /** @return true if the current iterate matches and is added. */
        bool addMatch();
        /** Note that a yield occurred. */
        void noteYield();
        /** @return true if there are enough results to return the first batch. */
        bool enoughForFirstBatch() const;
        /** @return true if there are enough results to return the full result set. */
        bool enoughTotalResults() const;
        /**
         * Callback when enough results have been read for the first batch, with potential handoff
         * to getMore.
         */
        void finishedFirstBatch();
        /**
         * Set the data portion of the supplied Message to a buffer containing the query results.
         * @return the number of results in the buffer.
         */
        long long handoff( Message &result );
        /** A chunk manager found at the beginning of the query. */
        ShardChunkManagerPtr chunkManager() const { return _chunkManager; }
    private:
        ShardChunkManagerPtr newChunkManager() const;
        shared_ptr<ExplainRecordingStrategy> newExplainRecordingStrategy
        ( const QueryPlan::Summary &queryPlan, const BSONObj &oldPlan ) const;
        shared_ptr<ResponseBuildStrategy> newResponseBuildStrategy
        ( const QueryPlan::Summary &queryPlan );
        bool currentMatches();
        bool chunkMatches();
        const ParsedQuery &_parsedQuery;
        shared_ptr<Cursor> _cursor;
        shared_ptr<QueryOptimizerCursor> _queryOptimizerCursor;
        BufBuilder _buf;
        ShardChunkManagerPtr _chunkManager;
        shared_ptr<ExplainRecordingStrategy> _explain;
        shared_ptr<ResponseBuildStrategy> _builder;
        long long _bufferedMatches;
    };

} // namespace mongo
