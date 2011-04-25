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
#include "query.h"
#include "../client/dbclient.h"
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
    void logOp(const char *opstr, const char *ns, const BSONObj& obj, BSONObj *patt = 0, bool *b = 0);

    void logKeepalive();

    /** puts obj in the oplog as a comment (a no-op).  Just for diags.
        convention is
          { msg : "text", ... }
    */
    void logOpComment(const BSONObj& obj);

    void oplogCheckCloseDatabase( Database * db );

    extern int __findingStartInitialTimeout; // configurable for testing

    class QueryPlan;
    
    class FindingStartCursor {
    public:
        FindingStartCursor( const QueryPlan & qp );
        bool done() const { return !_findingStart; }
        shared_ptr<Cursor> cRelease() { return _c; }
        void next();
        bool prepareToYield() {
            if ( _findingStartCursor ) {
                return _findingStartCursor->prepareToYield( _yieldData );
            }
            return true;
        }
        void recoverFromYield() {
            if ( _findingStartCursor ) {
                if ( !ClientCursor::recoverFromYield( _yieldData ) ) {
                    _findingStartCursor.reset( 0 );
                }
            }
        }
    private:
        enum FindingStartMode { Initial, FindExtent, InExtent };
        const QueryPlan &_qp;
        bool _findingStart;
        FindingStartMode _findingStartMode;
        auto_ptr< CoveredIndexMatcher > _matcher;
        Timer _findingStartTimer;
        ClientCursor::CleanupPointer _findingStartCursor;
        shared_ptr<Cursor> _c;
        ClientCursor::YieldData _yieldData;
        DiskLoc startLoc( const DiskLoc &rec );

        // should never have an empty extent in the oplog, so don't worry about that case
        DiskLoc prevLoc( const DiskLoc &rec );
        void createClientCursor( const DiskLoc &startLoc = DiskLoc() );
        void destroyClientCursor() {
            _findingStartCursor.reset( 0 );
        }
        void init();
    };

    void pretouchOperation(const BSONObj& op);
    void pretouchN(vector<BSONObj>&, unsigned a, unsigned b);

    /**
     * take an op and apply locally
     * used for applying from an oplog
     * @param fromRepl really from replication or for testing/internal/command/etc...
     */
    void applyOperation_inlock(const BSONObj& op , bool fromRepl = true );
}
