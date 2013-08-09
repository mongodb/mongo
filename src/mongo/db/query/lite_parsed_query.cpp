/*    Copyright 2013 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/db/query/lite_parsed_query.h"

#include "mongo/db/dbmessage.h"
#include "mongo/util/assert_util.h"

namespace mongo {

    // XXX: THIS DOESNT BELONG HERE
    /* We cut off further objects once we cross this threshold; thus, you might get
       a little bit more than this, it is a threshold rather than a limit.
    */
    static const int32_t MaxBytesToReturnToClientAtOnce = 4 * 1024 * 1024;

    LiteParsedQuery::LiteParsedQuery( QueryMessage& qm ) :
        _ns( qm.ns ),
        _ntoskip( qm.ntoskip ),
        _ntoreturn( qm.ntoreturn ),
        _options( qm.queryOptions ) {
        init( qm.query );
    }

    LiteParsedQuery::LiteParsedQuery( const char* ns,
                              int ntoskip,
                              int ntoreturn,
                              int queryoptions,
                              const BSONObj& query) :
        _ns( ns ),
        _ntoskip( ntoskip ),
        _ntoreturn( ntoreturn ),
        _options( queryoptions ) {
        init( query );
    }

    bool LiteParsedQuery::hasIndexSpecifier() const {
        return ! _hint.isEmpty() || ! _min.isEmpty() || ! _max.isEmpty();
    }

    // XXX THIS DOESNT BELONG HERE
    bool LiteParsedQuery::enoughForFirstBatch( int n , int len ) const {
        if ( _ntoreturn == 0 )
            return ( len > 1024 * 1024 ) || n >= 101;
        return n >= _ntoreturn || len > MaxBytesToReturnToClientAtOnce;
    }

    bool LiteParsedQuery::enough( int n ) const {
        if ( _ntoreturn == 0 )
            return false;
        return n >= _ntoreturn;
    }

    bool LiteParsedQuery::enoughForExplain( long long n ) const {
        if ( _wantMore || _ntoreturn == 0 ) {
            return false;
        }
        return n >= _ntoreturn;            
    }
    
    void LiteParsedQuery::init( const BSONObj& q ) {
        _reset();
        uassert(17032, "bad skip value in query", _ntoskip >= 0);
        
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
            _filter = e.embeddedObject().getOwned();
            _initTop( q );
        }
        else {
            _filter = q.getOwned();
        }

        //
        // Parse options that are valid for both queries and commands
        //

        // $readPreference
        _hasReadPref = q.hasField("$readPreference");

        // $maxTimeMS
        BSONElement maxTimeMSElt = q.getField("$maxTimeMS");
        if (!maxTimeMSElt.eoo()) {
            uassert(17033,
                    mongoutils::str::stream() <<
                        "$maxTimeMS must be a number type, instead found type: " <<
                        maxTimeMSElt.type(),
                    maxTimeMSElt.isNumber());
        }
        // If $maxTimeMS was not specified, _maxTimeMS is set to 0 (special value for "allow to
        // run indefinitely").
        long long maxTimeMSLongLong = maxTimeMSElt.safeNumberLong();
        uassert(17034,
                "$maxTimeMS out of range [0,2147483647]",
                maxTimeMSLongLong >= 0 && maxTimeMSLongLong <= INT_MAX);
        _maxTimeMS = static_cast<int>(maxTimeMSLongLong);
    }

    void LiteParsedQuery::_reset() {
        _wantMore = true;
        _explain = false;
        _snapshot = false;
        _returnKey = false;
        _showDiskLoc = false;
        _maxScan = 0;
    }

    /* This is for languages whose "objects" are not well ordered (JSON is well ordered).
     [ { a : ... } , { b : ... } ] -> { a : ..., b : ... }
     */
    static BSONObj transformOrderFromArrayFormat(BSONObj order) {
        /* note: this is slow, but that is ok as order will have very few pieces */
        BSONObjBuilder b;
        char p[2] = "0";
        
        while ( 1 ) {
            BSONObj j = order.getObjectField(p);
            if ( j.isEmpty() )
                break;
            BSONElement e = j.firstElement();
            uassert(17035, "bad order array", !e.eoo());
            uassert(17036, "bad order array [2]", e.isNumber());
            b.append(e);
            (*p)++;
            uassert(17037, "too many ordering elements", *p <= '9');
        }
        
        return b.obj();
    }

    void LiteParsedQuery::_initTop( const BSONObj& top ) {
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
                    uasserted(17038, "sort must be an object or array");
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
                    _hint = e.wrap();
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
            uassert(17039, "E12001 can't sort with $snapshot", _order.isEmpty() );
            uassert(17050, "E12002 can't use hint with $snapshot", _hint.isEmpty() );
        }
        
    }

} // namespace mongo
