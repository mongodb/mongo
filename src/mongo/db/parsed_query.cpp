// DEPRECATED
/*    Copyright 2009 10gen Inc.
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

#include "mongo/db/parsed_query.h"

#include <cstring> // for strstr

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/projection.h"
#include "mongo/db/ops/query.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/util/assert_util.h"

namespace mongo {

    ParsedQuery::ParsedQuery( QueryMessage& qm ) :
        _ns( qm.ns ),
        _ntoskip( qm.ntoskip ),
        _ntoreturn( qm.ntoreturn ),
        _options( qm.queryOptions ) {
        init( qm.query );
        initFields( qm.fields );
    }

    ParsedQuery::ParsedQuery( const char* ns,
                              int ntoskip,
                              int ntoreturn,
                              int queryoptions,
                              const BSONObj& query,
                              const BSONObj& fields ) :
        _ns( ns ),
        _ntoskip( ntoskip ),
        _ntoreturn( ntoreturn ),
        _options( queryoptions ) {
        init( query );
        initFields( fields );
    }

    bool ParsedQuery::hasIndexSpecifier() const {
        return ! _hint.isEmpty() || ! _min.isEmpty() || ! _max.isEmpty();
    }

    bool ParsedQuery::enoughForFirstBatch( int n , int len ) const {
        if ( _ntoreturn == 0 )
            return ( len > 1024 * 1024 ) || n >= 101;
        return n >= _ntoreturn || len > MaxBytesToReturnToClientAtOnce;
    }

    bool ParsedQuery::enough( int n ) const {
        if ( _ntoreturn == 0 )
            return false;
        return n >= _ntoreturn;
    }

    bool ParsedQuery::enoughForExplain( long long n ) const {
        if ( _wantMore || _ntoreturn == 0 ) {
            return false;
        }
        return n >= _ntoreturn;            
    }
    
    void ParsedQuery::init( const BSONObj& q ) {
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
            _filter = e.embeddedObject().getOwned();
            _initTop( q );
        }
        else {
            _filter = q.getOwned();
        }

        _hasReadPref = q.hasField(Query::ReadPrefField.name());
    }

    void ParsedQuery::_reset() {
        _wantMore = true;
        _explain = false;
        _snapshot = false;
        _returnKey = false;
        _showDiskLoc = false;
        _maxScan = 0;
        _maxTimeMS = 0;
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
            uassert( 10102 , "bad order array", !e.eoo());
            uassert( 10103 , "bad order array [2]", e.isNumber());
            b.append(e);
            (*p)++;
            uassert( 10104 , "too many ordering elements", *p <= '9');
        }
        
        return b.obj();
    }

    void ParsedQuery::_initTop( const BSONObj& top ) {
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
                if ( strcmp( "explain" , name ) == 0 ) {
                    _explain = e.trueValue();
                }
                else if ( strcmp( "snapshot" , name ) == 0 ) {
                    _snapshot = e.trueValue();
                }
                else if ( strcmp( "min" , name ) == 0 ) {
                    _min = e.embeddedObject();
                }
                else if ( strcmp( "max" , name ) == 0 ) {
                    _max = e.embeddedObject();
                }
                else if ( strcmp( "hint" , name ) == 0 ) {
                    _hint = e.wrap();
                }
                else if ( strcmp( "returnKey" , name ) == 0 ) {
                    _returnKey = e.trueValue();
                }
                else if ( strcmp( "maxScan" , name ) == 0 ) {
                    _maxScan = e.numberInt();
                }
                else if ( strcmp( "showDiskLoc" , name ) == 0 ) {
                    _showDiskLoc = e.trueValue();
                }
                else if ( strcmp( "maxTimeMS" , name ) == 0 ) {
                    StatusWith<int> maxTimeMS = LiteParsedQuery::parseMaxTimeMS(e);
                    uassert(17131,
                            maxTimeMS.getStatus().reason(),
                            maxTimeMS.isOK());
                    _maxTimeMS = maxTimeMS.getValue();
                }
                else if ( strcmp( "comment" , name ) == 0 ) {
                    ; // no-op
                }
            }
        }
        
        if ( _snapshot ) {
            uassert( 12001 , "E12001 can't sort with $snapshot", _order.isEmpty() );
            uassert( 12002 , "E12002 can't use hint with $snapshot", _hint.isEmpty() );
        }
        
    }

    void ParsedQuery::initFields( const BSONObj& fields ) {
        if ( fields.isEmpty() )
            return;
        _fields.reset( new Projection() );
        _fields->init( fields.getOwned() );
    }

    
} // namespace mongo
