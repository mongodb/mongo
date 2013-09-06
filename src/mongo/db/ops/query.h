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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include "mongo/pch.h"

#include "mongo/db/diskloc.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/explain.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/collection_metadata.h"
#include "mongo/util/net/message.h"

// struct QueryOptions, QueryResult, QueryResultFlags in:

namespace mongo {

    class CurOp;
    class ParsedQuery;
    class QueryOptimizerCursor;
    struct QueryPlanSummary;

    extern const int32_t MaxBytesToReturnToClientAtOnce;
    
    /**
     * Return a batch of results from a client OP_GET_MORE request.
     * 'cursorid' - The id of the cursor producing results.
     * 'isCursorAuthorized' - Set to true after a cursor with id 'cursorid' is authorized for use.
     */
    QueryResult* processGetMore(const char* ns,
                                int ntoreturn,
                                long long cursorid,
                                CurOp& op,
                                int pass,
                                bool& exhaust,
                                bool* isCursorAuthorized);

    string runQuery(Message& m, QueryMessage& q, CurOp& curop, Message &result);

    /** Exception indicating that a query should be retried from the beginning. */
    class QueryRetryException : public DBException {
    public:
        QueryRetryException() : DBException( "query retry exception" , 16083 ) {
            return;
            massert( 16083, "reserve 16083", true ); // Reserve 16083.
        }
    };

    /** Metadata about matching and loading a single candidate result document from a Cursor. */
    struct ResultDetails {
        ResultDetails();
        MatchDetails matchDetails; // Details on how the Matcher matched the query.
        bool match;                // Matched the query, was not a dup, was not skipped etc.
        bool orderedMatch;         // _match and belonged to an ordered query plan.
        bool loadedRecord;         // Record was loaded (to match or return the document).
        bool chunkSkip;            // Did not belong to an owned chunk range.
    };

    /** Interface for recording events that contribute to explain results. */
    class ExplainRecordingStrategy {
    public:
        ExplainRecordingStrategy( const ExplainQueryInfo::AncillaryInfo &ancillaryInfo );
        virtual ~ExplainRecordingStrategy() {}
        /** Note information about a single query plan. */
        virtual void notePlan( bool scanAndOrder, bool indexOnly ) {}
        /** Note an iteration of the query. */
        virtual void noteIterate( const ResultDetails& resultDetails ) {}
        /** Note that the query yielded. */
        virtual void noteYield() {}
        /** @return number of ordered matches noted. */
        virtual long long orderedMatches() const { return 0; }
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
    
    class MatchCountingExplainStrategy : public ExplainRecordingStrategy {
    public:
        MatchCountingExplainStrategy( const ExplainQueryInfo::AncillaryInfo &ancillaryInfo );
    protected:
        virtual void _noteIterate( const ResultDetails& resultDetails ) = 0;
    private:
        virtual void noteIterate( const ResultDetails& resultDetails );
        virtual long long orderedMatches() const { return _orderedMatches; }
        long long _orderedMatches;
    };
    
    /** Record explain events for a simple cursor representing a single clause and plan. */
    class SimpleCursorExplainStrategy : public MatchCountingExplainStrategy {
    public:
        SimpleCursorExplainStrategy( const ExplainQueryInfo::AncillaryInfo &ancillaryInfo,
                                    const shared_ptr<Cursor> &cursor );
    private:
        virtual void notePlan( bool scanAndOrder, bool indexOnly );
        virtual void _noteIterate( const ResultDetails& resultDetails );
        virtual void noteYield();
        virtual shared_ptr<ExplainQueryInfo> _doneQueryInfo();
        shared_ptr<Cursor> _cursor;
        shared_ptr<ExplainSinglePlanQueryInfo> _explainInfo;
    };
    
    /**
     * Record explain events for a QueryOptimizerCursor, which may record some explain information
     * for multiple clauses and plans through an internal implementation.
     */
    class QueryOptimizerCursorExplainStrategy : public MatchCountingExplainStrategy {
    public:
        QueryOptimizerCursorExplainStrategy( const ExplainQueryInfo::AncillaryInfo &ancillaryInfo,
                                            const shared_ptr<QueryOptimizerCursor> &cursor );
    private:
        virtual void _noteIterate( const ResultDetails& resultDetails );
        virtual void noteYield();
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
                              BufBuilder &buf );
        virtual ~ResponseBuildStrategy() {}
        /**
         * Handle the current iterate of the supplied cursor as a (possibly duplicate) match.
         * @return true if a match is found.
         * @param resultDetails details of how the result is matched and loaded.
         */
        virtual bool handleMatch( ResultDetails* resultDetails ) = 0;

        /**
         * Write all matches into the buffer, overwriting existing data.
         * @return number of matches written, or -1 if no op.
         */
        virtual int rewriteMatches() { return -1; }
        /** @return the number of matches that have been written to the buffer. */
        virtual int bufferedMatches() const = 0;
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
         * @param allowCovered enable covered index support.
         * @param resultDetails details of how the result is loaded.
         */
        BSONObj current( bool allowCovered, ResultDetails* resultDetails ) const;
        const ParsedQuery &_parsedQuery;
        shared_ptr<Cursor> _cursor;
        shared_ptr<QueryOptimizerCursor> _queryOptimizerCursor;
        BufBuilder &_buf;
    };

    /** Build strategy for a cursor returning in order results. */
    class OrderedBuildStrategy : public ResponseBuildStrategy {
    public:
        OrderedBuildStrategy( const ParsedQuery &parsedQuery, const shared_ptr<Cursor> &cursor,
                             BufBuilder &buf );
        virtual bool handleMatch( ResultDetails* resultDetails );
        virtual int bufferedMatches() const { return _bufferedMatches; }
    private:
        int _skip;
        int _bufferedMatches;
    };
    
    class ScanAndOrder;
    
    /** Build strategy for a cursor returning out of order results. */
    class ReorderBuildStrategy : public ResponseBuildStrategy {
    public:
        static ReorderBuildStrategy* make( const ParsedQuery& parsedQuery,
                                           const shared_ptr<Cursor>& cursor,
                                           BufBuilder& buf,
                                           const QueryPlanSummary& queryPlan );
        virtual bool handleMatch( ResultDetails* resultDetails );
        /** Handle a match without performing deduping. */
        void _handleMatchNoDedup( ResultDetails* resultDetails );
        virtual int rewriteMatches();
        virtual int bufferedMatches() const { return _bufferedMatches; }
    private:
        ReorderBuildStrategy( const ParsedQuery& parsedQuery,
                              const shared_ptr<Cursor>& cursor,
                              BufBuilder& buf );
        void init( const QueryPlanSummary& queryPlan );
        ScanAndOrder *newScanAndOrder( const QueryPlanSummary &queryPlan ) const;
        shared_ptr<ScanAndOrder> _scanAndOrder;
        int _bufferedMatches;
    };

    /** Helper class for deduping DiskLocs */
    class DiskLocDupSet {
    public:
        /** @return true if dup, otherwise return false and insert. */
        bool getsetdup( const DiskLoc &loc ) {
            pair<set<DiskLoc>::iterator, bool> p = _dups.insert(loc);
            return !p.second;
        }
    private:
        set<DiskLoc> _dups;
    };

    /**
     * Build strategy for a QueryOptimizerCursor containing some in order and some out of order
     * candidate plans.
     */
    class HybridBuildStrategy : public ResponseBuildStrategy {
    public:
        static HybridBuildStrategy* make( const ParsedQuery& parsedQuery,
                                          const shared_ptr<QueryOptimizerCursor>& cursor,
                                          BufBuilder& buf );
    private:
        HybridBuildStrategy( const ParsedQuery &parsedQuery,
                            const shared_ptr<QueryOptimizerCursor> &cursor,
                            BufBuilder &buf );
        void init();
        virtual bool handleMatch( ResultDetails* resultDetails );
        virtual int rewriteMatches();
        virtual int bufferedMatches() const;
        virtual void finishedFirstBatch();
        bool handleReorderMatch( ResultDetails* resultDetails );
        DiskLocDupSet _scanAndOrderDups;
        OrderedBuildStrategy _orderedBuild;
        scoped_ptr<ReorderBuildStrategy> _reorderBuild;
        bool _reorderedMatches;
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
        static QueryResponseBuilder *make( const ParsedQuery &parsedQuery,
                                          const shared_ptr<Cursor> &cursor,
                                          const QueryPlanSummary &queryPlan,
                                          const BSONObj &oldPlan );
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
        int handoff( Message &result );
        /** Metadata found at the beginning of the query. */
        CollectionMetadataPtr collMetadata() const { return _collMetadata; }

    private:
        QueryResponseBuilder( const ParsedQuery &parsedQuery, const shared_ptr<Cursor> &cursor );
        void init( const QueryPlanSummary &queryPlan, const BSONObj &oldPlan );

        CollectionMetadataPtr newCollMetadata() const;
        shared_ptr<ExplainRecordingStrategy> newExplainRecordingStrategy
        ( const QueryPlanSummary &queryPlan, const BSONObj &oldPlan ) const;
        shared_ptr<ResponseBuildStrategy> newResponseBuildStrategy
        ( const QueryPlanSummary &queryPlan );
        /**
         * @return true if the cursor's document matches the query.
         * @param resultDetails describes how the document was matched and loaded.
         */
        bool currentMatches( ResultDetails* resultDetails );
        /**
         * @return true if the cursor's document is in a valid chunk range.
         * @param resultDetails describes how the document was matched and loaded.
         */
        bool chunkMatches( ResultDetails* resultDetails );
        const ParsedQuery &_parsedQuery;
        shared_ptr<Cursor> _cursor;
        shared_ptr<QueryOptimizerCursor> _queryOptimizerCursor;
        BufBuilder _buf;
        CollectionMetadataPtr _collMetadata;
        shared_ptr<ExplainRecordingStrategy> _explain;
        shared_ptr<ResponseBuildStrategy> _builder;
    };

} // namespace mongo
