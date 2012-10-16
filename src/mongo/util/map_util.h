// "mongo/util/map_util.h"

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

namespace mongo {

/*
 *  If "myMap" contains "key", returns "myMap[key]".  Otherwise, returns "defaultValue."
 */
template <typename M, typename K, typename V>
V mapFindWithDefault(const M& myMap, const K& key, const V& defaultValue) {
    typename M::const_iterator it = myMap.find(key);
    if(it == myMap.end())
        return defaultValue;
    return it->second;
}

} // end namespace
