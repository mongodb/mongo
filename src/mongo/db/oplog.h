// oplog.h - writing to and reading from oplog

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

/*

     local.oplog.$main is the default
*/

#pragma once

#include "pdfile.h"
#include "db.h"
#include "dbhelpers.h"
#include "clientcursor.h"
#include "../util/optime.h"
#include "../util/timer.h"

namespace mongo {

    void createOplog();

    void _logOpObjRS(const BSONObj& op);

    /** Write operation to the log (local.oplog.$main)

       @param opstr
        "i" insert
        "u" update
        "d" delete
        "c" db cmd
        "n" no-op
        "db" declares presence of a database (ns is set to the db name + '.')

       See _logOp() in oplog.cpp for more details.
    */
    void logOp( const char *opstr, const char *ns, const BSONObj& obj, BSONObj *patt = 0, bool *b = 0, bool fromMigrate = false );

    void logKeepalive();

    /** puts obj in the oplog as a comment (a no-op).  Just for diags.
        convention is
          { msg : "text", ... }
    */
    void logOpComment(const BSONObj& obj);

    void oplogCheckCloseDatabase( Database * db );

    class QueryPlan;
    
    /** Implements an optimized procedure for finding the first op in the oplog. */
    class FindingStartCursor {
    public:

        /**
         * The cursor will attempt to find the first op in the oplog matching the
         * 'ts' field of the qp's query.
         */
        static FindingStartCursor *make( const QueryPlan &qp );

        /** @return true if the first matching op in the oplog has been found. */
        bool done() const { return !_findingStart; }

        /** @return cursor pointing to the first matching op, if done(). */
        shared_ptr<Cursor> cursor() { verify( done() ); return _c; }

        /** Iterate the cursor, to continue trying to find matching op. */
        void next();

        /** Yield cursor, if not done(). */
        bool prepareToYield() {
            if ( _findingStartCursor ) {
                return _findingStartCursor->prepareToYield( _yieldData );
            }
            return false;
        }
        
        /** Recover from cursor yield. */
        void recoverFromYield() {
            if ( _findingStartCursor ) {
                if ( !ClientCursor::recoverFromYield( _yieldData ) ) {
                    _findingStartCursor.reset( 0 );
                    msgassertedNoTrace( 15889, "FindingStartCursor::recoverFromYield() failed to recover" );
                }
            }
        }
        
        /**
         * @return a BasicCursor constructed using a FindingStartCursor with the provided query and
         * order parameters.
         * @yields the db lock.
         * @asserts on yield recovery failure.
         */
        static shared_ptr<Cursor> getCursor( const char *ns, const BSONObj &query, const BSONObj &order );

        /**
         * @return the first record of the first nonempty extent preceding the extent containing
         *     @param rec, or DiskLoc() if there is no such record or the beginning of the
         *     collection is reached.
         * public for testing
         */
        DiskLoc prevExtentFirstLoc( const DiskLoc& rec ) const;

        /** For testing only. */

        static int getInitialTimeout() { return _initialTimeout; }
        static void setInitialTimeout( int timeout ) { _initialTimeout = timeout; }

    private:
        FindingStartCursor( const QueryPlan &qp );
        void init();

        enum FindingStartMode { Initial, FindExtent, InExtent };
        const QueryPlan &_qp;
        bool _findingStart;
        FindingStartMode _findingStartMode;
        auto_ptr< CoveredIndexMatcher > _matcher;
        Timer _findingStartTimer;
        ClientCursor::Holder _findingStartCursor;
        shared_ptr<Cursor> _c;
        ClientCursor::YieldData _yieldData;
        static int _initialTimeout;

        /** @return the first record of the extent containing @param rec. */
        DiskLoc extentFirstLoc( const DiskLoc &rec );

        void createClientCursor( const DiskLoc &startLoc = DiskLoc() );
        void destroyClientCursor() {
            _findingStartCursor.reset( 0 );
        }
        bool firstDocMatchesOrEmpty() const;
    };

    class Sync {
    protected:
        string hn;
    public:
        Sync(const string& hostname) : hn(hostname) {}
        virtual ~Sync() {}
        virtual BSONObj getMissingDoc(const BSONObj& o);

        /**
         * If applyOperation_inlock should be called again after an update fails.
         */
        virtual bool shouldRetry(const BSONObj& o);
        void setHostname(const string& hostname);
    };

    void pretouchOperation(const BSONObj& op);
    void pretouchN(vector<BSONObj>&, unsigned a, unsigned b);

    /**
     * take an op and apply locally
     * used for applying from an oplog
     * @param fromRepl really from replication or for testing/internal/command/etc...
     * Returns if the op was an update that could not be applied (true on failure)
     */
    bool applyOperation_inlock(const BSONObj& op, bool fromRepl = true, bool convertUpdateToUpsert = false);
}
