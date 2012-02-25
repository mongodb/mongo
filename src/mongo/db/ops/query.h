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
#include "../queryoptimizercursor.h"
#include "../queryoptimizer.h"
#include "../../s/d_chunk_manager.h"

// struct QueryOptions, QueryResult, QueryResultFlags in:
#include "../../client/dbclient.h"

namespace mongo {

    QueryResult* processGetMore(const char *ns, int ntoreturn, long long cursorid , CurOp& op, int pass, bool& exhaust);

    const char * runQuery(Message& m, QueryMessage& q, CurOp& curop, Message &result);

    class QueryRetryException : public DBException {
    public:
        QueryRetryException() : DBException( "query retry exception" , 16083 ) {}
    };

    class ExplainRecordingStrategy {
    public:
        ExplainRecordingStrategy( const ExplainQueryInfo::AncillaryInfo &ancillaryInfo );
        virtual ~ExplainRecordingStrategy() {}
        virtual void notePlan( bool scanAndOrder, bool indexOnly ) {}
        virtual void noteIterate( bool match, bool loadedObject, bool chunkSkip ) {}
        virtual void noteYield() {}
        shared_ptr<ExplainQueryInfo> doneQueryInfo();
    protected:
        virtual shared_ptr<ExplainQueryInfo> _doneQueryInfo() = 0;
    private:
        ExplainQueryInfo::AncillaryInfo _ancillaryInfo;
    };
    
    class NoExplainStrategy : public ExplainRecordingStrategy {
    public:
        NoExplainStrategy();
    private:
        virtual shared_ptr<ExplainQueryInfo> _doneQueryInfo();
    };
    
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
    
    class QueryOptimizerCursorExplainStrategy : public ExplainRecordingStrategy {
    public:
        QueryOptimizerCursorExplainStrategy( const ExplainQueryInfo::AncillaryInfo &ancillaryInfo,
                                            const shared_ptr<QueryOptimizerCursor> cursor );
    private:
        virtual void noteIterate( bool match, bool loadedObject, bool chunkSkip );
        virtual shared_ptr<ExplainQueryInfo> _doneQueryInfo();
        shared_ptr<QueryOptimizerCursor> _cursor;
    };

    class ResponseBuildStrategy {
    public:
        ResponseBuildStrategy( const ParsedQuery &parsedQuery, const shared_ptr<Cursor> &cursor,
                              BufBuilder &buf, const QueryPlan::Summary &queryPlan );
        virtual ~ResponseBuildStrategy() {}
        virtual bool handleMatch() = 0;
        virtual int rewriteMatches() { return -1; }
        void resetBuf();
    protected:
        BSONObj current( bool allowCovered ) const;
        const ParsedQuery &_parsedQuery;
        shared_ptr<Cursor> _cursor;
        shared_ptr<QueryOptimizerCursor> _queryOptimizerCursor;
        BufBuilder &_buf;
    private:
        const Projection::KeyOnly *keyFieldsOnly() const;
        shared_ptr<Projection::KeyOnly> _planKeyFieldsOnly;
    };

    class OrderedBuildStrategy : public ResponseBuildStrategy {
    public:
        OrderedBuildStrategy( const ParsedQuery &parsedQuery, const shared_ptr<Cursor> &cursor,
                             BufBuilder &buf, const QueryPlan::Summary &queryPlan );
        virtual bool handleMatch();
    private:
        long long _skip;        
    };
    
    class ScanAndOrder;
    
    class ReorderBuildStrategy : public ResponseBuildStrategy {
    public:
        ReorderBuildStrategy( const ParsedQuery &parsedQuery,
                             const shared_ptr<Cursor> &cursor,
                             BufBuilder &buf,
                             const QueryPlan::Summary &queryPlan );
        virtual bool handleMatch();
        bool _handleMatchNoDedup();
        virtual int rewriteMatches();
    private:
        ScanAndOrder *newScanAndOrder( const QueryPlan::Summary &queryPlan ) const;
        shared_ptr<ScanAndOrder> _scanAndOrder;
    };

    class HybridBuildStrategy : public ResponseBuildStrategy {
    public:
        HybridBuildStrategy( const ParsedQuery &parsedQuery,
                            const shared_ptr<QueryOptimizerCursor> &cursor,
                            BufBuilder &buf );
        virtual bool handleMatch();
        virtual int rewriteMatches();
    private:
        void handleReorderMatch();
        bool handleOrderedMatch();
        SmallDupSet _scanAndOrderDups;
        OrderedBuildStrategy _orderedBuild;
        ReorderBuildStrategy _reorderBuild;
    };

    class QueryResponseBuilder {
    public:
        QueryResponseBuilder( const ParsedQuery &parsedQuery, const shared_ptr<Cursor> &cursor,
                             const QueryPlan::Summary &queryPlan, const BSONObj &oldPlan );
        bool addMatch();
        void noteYield();
        bool enoughForFirstBatch() const;
        bool enoughTotalResults() const;
        void finishedFirstBatch();
        long long handoff( Message &result );
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
