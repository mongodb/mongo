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

#pragma once

#include <boost/shared_ptr.hpp>

#include "mongo/db/jsobj.h"

namespace mongo {

    class QueryMessage;
    class Projection;

    /**
     * this represents a total user query
     * includes fields from the query message, both possible query levels
     * parses everything up front
     */
    class ParsedQuery : boost::noncopyable {
    public:

        ParsedQuery( QueryMessage& qm );

        ParsedQuery( const char* ns,
                     int ntoskip,
                     int ntoreturn,
                     int queryoptions,
                     const BSONObj& query,
                     const BSONObj& fields );
        
        const char * ns() const { return _ns; }
        bool isLocalDB() const { return strncmp( _ns, "local.", 6 ) == 0; }
        
        const BSONObj& getFilter() const { return _filter; }
        Projection* getFields() const { return _fields.get(); }
        shared_ptr<Projection> getFieldPtr() const { return _fields; }
        
        int getSkip() const { return _ntoskip; }
        int getNumToReturn() const { return _ntoreturn; }
        bool wantMore() const { return _wantMore; }
        int getOptions() const { return _options; }
        bool hasOption( int x ) const { return ( x & _options ) != 0; }
        bool hasReadPref() const { return _hasReadPref; }
        
        bool isExplain() const { return _explain; }
        bool isSnapshot() const { return _snapshot; }
        bool returnKey() const { return _returnKey; }
        bool showDiskLoc() const { return _showDiskLoc; }
        
        const BSONObj& getMin() const { return _min; }
        const BSONObj& getMax() const { return _max; }
        const BSONObj& getOrder() const { return _order; }
        const BSONObj& getHint() const { return _hint; }
        int getMaxScan() const { return _maxScan; }
        int getMaxTimeMS() const { return _maxTimeMS; }

        bool hasIndexSpecifier() const;
        
        /* if ntoreturn is zero, we return up to 101 objects.  on the subsequent getmore, there
         is only a size limit.  The idea is that on a find() where one doesn't use much results,
         we don't return much, but once getmore kicks in, we start pushing significant quantities.
         
         The n limit (vs. size) is important when someone fetches only one small field from big
         objects, which causes massive scanning server-side.
         */
        bool enoughForFirstBatch( int n, int len ) const;
        
        bool enough( int n ) const;
        
        bool enoughForExplain( long long n ) const;
        
    private:
        void init( const BSONObj& q );
        
        void _reset();
        
        void _initTop( const BSONObj& top );

        void initFields( const BSONObj& fields );
        
        const char* const _ns;
        const int _ntoskip;
        int _ntoreturn;
        BSONObj _filter;
        BSONObj _order;
        const int _options;
        shared_ptr<Projection> _fields;
        bool _wantMore;
        bool _explain;
        bool _snapshot;
        bool _returnKey;
        bool _showDiskLoc;
        bool _hasReadPref;
        BSONObj _min;
        BSONObj _max;
        BSONObj _hint;
        int _maxScan;
        int _maxTimeMS;
    };

} // namespace mongo
