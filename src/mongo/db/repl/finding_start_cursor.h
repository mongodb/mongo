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


#include <boost/shared_ptr.hpp>
#include <memory>

#include "mongo/db/clientcursor.h"
#include "mongo/db/diskloc.h"

namespace mongo {
    
    class QueryPlan;
    class Cursor;
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
        ClientCursorHolder _findingStartCursor;
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

}
