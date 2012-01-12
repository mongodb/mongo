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
#include "../projection.h"

// struct QueryOptions, QueryResult, QueryResultFlags in:
#include "../../client/dbclient.h"

namespace mongo {

    extern const int MaxBytesToReturnToClientAtOnce;

    QueryResult* processGetMore(const char *ns, int ntoreturn, long long cursorid , CurOp& op, int pass, bool& exhaust);

    const char * runQuery(Message& m, QueryMessage& q, CurOp& curop, Message &result);

    /* This is for languages whose "objects" are not well ordered (JSON is well ordered).
       [ { a : ... } , { b : ... } ] -> { a : ..., b : ... }
    */
    inline BSONObj transformOrderFromArrayFormat(BSONObj order) {
        /* note: this is slow, but that is ok as order will have very few pieces */
        BSONObjBuilder b;
        char p[2] = "0";

        while ( 1 ) {
            BSONObj j = order.getObjectField(p);
            if ( j.isEmpty() )
                break;
            BSONElement e = j.firstElement();
            uassert( 10102 , "bad order array", !e.eoo());
            uassert( 10103 , "bad order array [2]", e.isNumber());
            b.append(e);
            (*p)++;
            uassert( 10104 , "too many ordering elements", *p <= '9');
        }

        return b.obj();
    }

    /**
     * this represents a total user query
     * includes fields from the query message, both possible query levels
     * parses everything up front
     */
    class ParsedQuery : boost::noncopyable {
    public:
        ParsedQuery( QueryMessage& qm )
            : _ns( qm.ns ) , _ntoskip( qm.ntoskip ) , _ntoreturn( qm.ntoreturn ) , _options( qm.queryOptions ) {
            init( qm.query );
            initFields( qm.fields );
        }
        ParsedQuery( const char* ns , int ntoskip , int ntoreturn , int queryoptions , const BSONObj& query , const BSONObj& fields )
            : _ns( ns ) , _ntoskip( ntoskip ) , _ntoreturn( ntoreturn ) , _options( queryoptions ) {
            init( query );
            initFields( fields );
        }

        const char * ns() const { return _ns; }
        bool isLocalDB() const { return strncmp(_ns, "local.", 6) == 0; }

        const BSONObj& getFilter() const { return _filter; }
        Projection* getFields() const { return _fields.get(); }
        shared_ptr<Projection> getFieldPtr() const { return _fields; }

        int getSkip() const { return _ntoskip; }
        int getNumToReturn() const { return _ntoreturn; }
        bool wantMore() const { return _wantMore; }
        int getOptions() const { return _options; }
        bool hasOption( int x ) const { return x & _options; }

        bool isExplain() const { return _explain; }
        bool isSnapshot() const { return _snapshot; }
        bool returnKey() const { return _returnKey; }
        bool showDiskLoc() const { return _showDiskLoc; }

        const BSONObj& getMin() const { return _min; }
        const BSONObj& getMax() const { return _max; }
        const BSONObj& getOrder() const { return _order; }
        const BSONElement& getHint() const { return _hint; }
        int getMaxScan() const { return _maxScan; }

        bool couldBeCommand() const {
            /* we assume you are using findOne() for running a cmd... */
            return _ntoreturn == 1 && strstr( _ns , ".$cmd" );
        }

        bool hasIndexSpecifier() const {
            return ! _hint.eoo() || ! _min.isEmpty() || ! _max.isEmpty();
        }

        /* if ntoreturn is zero, we return up to 101 objects.  on the subsequent getmore, there
           is only a size limit.  The idea is that on a find() where one doesn't use much results,
           we don't return much, but once getmore kicks in, we start pushing significant quantities.

           The n limit (vs. size) is important when someone fetches only one small field from big
           objects, which causes massive scanning server-side.
        */
        bool enoughForFirstBatch( int n , int len ) const {
            if ( _ntoreturn == 0 )
                return ( len > 1024 * 1024 ) || n >= 101;
            return n >= _ntoreturn || len > MaxBytesToReturnToClientAtOnce;
        }

        bool enough( int n ) const {
            if ( _ntoreturn == 0 )
                return false;
            return n >= _ntoreturn;
        }

    private:
        void init( const BSONObj& q ) {
            _reset();
            uassert( 10105 , "bad skip value in query", _ntoskip >= 0);

            if ( _ntoreturn < 0 ) {
                /* _ntoreturn greater than zero is simply a hint on how many objects to send back per
                   "cursor batch".
                   A negative number indicates a hard limit.
                */
                _wantMore = false;
                _ntoreturn = -_ntoreturn;
            }


            BSONElement e = q["query"];
            if ( ! e.isABSONObj() )
                e = q["$query"];

            if ( e.isABSONObj() ) {
                _filter = e.embeddedObject();
                _initTop( q );
            }
            else {
                _filter = q;
            }
        }

        void _reset() {
            _wantMore = true;
            _explain = false;
            _snapshot = false;
            _returnKey = false;
            _showDiskLoc = false;
            _maxScan = 0;
        }

        void _initTop( const BSONObj& top ) {
            BSONObjIterator i( top );
            while ( i.more() ) {
                BSONElement e = i.next();
                const char * name = e.fieldName();

                if ( strcmp( "$orderby" , name ) == 0 ||
                        strcmp( "orderby" , name ) == 0 ) {
                    if ( e.type() == Object ) {
                        _order = e.embeddedObject();
                    }
                    else if ( e.type() == Array ) {
                        _order = transformOrderFromArrayFormat( _order );
                    }
                    else {
                        uasserted(13513, "sort must be an object or array");
                    }
                    continue;
                }

                if( *name == '$' ) {
                    name++;
                    if ( strcmp( "explain" , name ) == 0 )
                        _explain = e.trueValue();
                    else if ( strcmp( "snapshot" , name ) == 0 )
                        _snapshot = e.trueValue();
                    else if ( strcmp( "min" , name ) == 0 )
                        _min = e.embeddedObject();
                    else if ( strcmp( "max" , name ) == 0 )
                        _max = e.embeddedObject();
                    else if ( strcmp( "hint" , name ) == 0 )
                        _hint = e;
                    else if ( strcmp( "returnKey" , name ) == 0 )
                        _returnKey = e.trueValue();
                    else if ( strcmp( "maxScan" , name ) == 0 )
                        _maxScan = e.numberInt();
                    else if ( strcmp( "showDiskLoc" , name ) == 0 )
                        _showDiskLoc = e.trueValue();
                    else if ( strcmp( "comment" , name ) == 0 ) {
                        ; // no-op
                    }
                }
            }

            if ( _snapshot ) {
                uassert( 12001 , "E12001 can't sort with $snapshot", _order.isEmpty() );
                uassert( 12002 , "E12002 can't use hint with $snapshot", _hint.eoo() );
            }

        }

        void initFields( const BSONObj& fields ) {
            if ( fields.isEmpty() )
                return;
            _fields.reset( new Projection() );
            _fields->init( fields );
        }

        const char * const _ns;
        const int _ntoskip;
        int _ntoreturn;
        BSONObj _filter;
        BSONObj _order;
        const int _options;
        shared_ptr< Projection > _fields;
        bool _wantMore;
        bool _explain;
        bool _snapshot;
        bool _returnKey;
        bool _showDiskLoc;
        BSONObj _min;
        BSONObj _max;
        BSONElement _hint;
        int _maxScan;
    };


} // namespace mongo


