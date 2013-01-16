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
     * NOTE that an OwnedPointerMap<K,T> wraps an std::map<K,T*>.
     */
    template<class K, class T>
    class OwnedPointerMap {
        MONGO_DISALLOW_COPYING(OwnedPointerMap);

    public:
        OwnedPointerMap();
        ~OwnedPointerMap();

        /** Access the map. */
        const std::map<K, T*>& map() { return _map; }
        std::map<K, T*>& mutableMap() { return _map; }

        void clear();

    private:
        std::map<K, T*> _map;
    };

    template<class K, class T>
    OwnedPointerMap<K, T>::OwnedPointerMap() {
    }

    template<class K, class T>
    OwnedPointerMap<K, T>::~OwnedPointerMap() {
        clear();
    }

    template<class K, class T>
    void OwnedPointerMap<K, T>::clear() {
        for( typename std::map<K, T*>::iterator i = _map.begin(); i != _map.end(); ++i ) {
            delete i->second;
        }
        _map.clear();
    }

} // namespace mongo
