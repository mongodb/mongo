// collection_info_cache.h

/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/index_set.h"
#include "mongo/db/querypattern.h"


namespace mongo {

    class Collection;

    /**
     * this is for storing things that you want to cache about a single collection
     * life cycle is managed for you from inside Collection
     */
    class CollectionInfoCache {
    public:

        CollectionInfoCache( Collection* collection );

        /*
         * resets entire cache state
         */
        void reset();

        // -------------------

        /* get set of index keys for this namespace.  handy to quickly check if a given
           field is indexed (Note it might be a secondary component of a compound index.)
        */
        const IndexPathSet& indexKeys() {
            if ( !_keysComputed )
                computeIndexKeys();
            return _indexedPaths;
        }

        // ---------------------

        void addedIndex() { reset(); }

        void clearQueryCache();

        /* you must notify the cache if you are doing writes, as query plan utility will change */
        void notifyOfWriteOp();

        CachedQueryPlan cachedQueryPlanForPattern( const QueryPattern &pattern );

        void registerCachedQueryPlanForPattern( const QueryPattern &pattern,
                                                const CachedQueryPlan &cachedQueryPlan );

    private:

        Collection* _collection; // not owned

        // ---  index keys cache
        bool _keysComputed;
        IndexPathSet _indexedPaths;

        void computeIndexKeys();

        // --- for old query optimizer

        void _clearQueryCache_inlock();

        mutex _qcCacheMutex;
        int _qcWriteCount;
        std::map<QueryPattern,CachedQueryPlan> _qcCache;

    };

}
