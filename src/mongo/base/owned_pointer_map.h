/*    Copyright 2012 10gen Inc.
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

#include <map>

#include "mongo/base/disallow_copying.h"

namespace mongo {

    /**
     * An std::map wrapper that deletes pointers within a vector on destruction.  The objects
     * referenced by the vector's pointers are 'owned' by an object of this class.
     * NOTE that an OwnedPointerMap<K,T,Compare> wraps an std::map<K,T*,Compare>.
     */
    template<class K, class T, class Compare = std::less<K> >
    class OwnedPointerMap {
        MONGO_DISALLOW_COPYING(OwnedPointerMap);

    public:
        typedef typename std::map<K, T*, Compare> MapType;

        OwnedPointerMap();
        ~OwnedPointerMap();

        /** Access the map. */
        const MapType& map() { return _map; }
        MapType& mutableMap() { return _map; }

        void clear();

    private:
        MapType _map;
    };

    template<class K, class T, class Compare>
    OwnedPointerMap<K, T, Compare>::OwnedPointerMap() {
    }

    template<class K, class T, class Compare>
    OwnedPointerMap<K, T, Compare>::~OwnedPointerMap() {
        clear();
    }

    template<class K, class T, class Compare>
    void OwnedPointerMap<K, T, Compare>::clear() {
        for( typename MapType::iterator i = _map.begin(); i != _map.end(); ++i ) {
            delete i->second;
        }
        _map.clear();
    }

} // namespace mongo
