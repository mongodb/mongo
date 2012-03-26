// lru-ish map.h

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

#include "../pch.h"
#include "../util/goodies.h"

namespace mongo {

    /* Your K object must define:
         int hash() - must always return > 0.
         operator==
    */

    template <class K, class V, int MaxChain>
    class LRUishMap {
    public:
        LRUishMap(int _n) {
            n = nextPrime(_n);
            keys = new K[n];
            hashes = new int[n];
            for ( int i = 0; i < n; i++ ) hashes[i] = 0;
        }
        ~LRUishMap() {
            delete[] keys;
            delete[] hashes;
        }

        int _find(const K& k, bool& found) {
            int h = k.hash();
            verify( h > 0 );
            int j = h % n;
            int first = j;
            for ( int i = 0; i < MaxChain; i++ ) {
                if ( hashes[j] == h ) {
                    if ( keys[j] == k ) {
                        found = true;
                        return j;
                    }
                }
                else if ( hashes[j] == 0 ) {
                    found = false;
                    return j;
                }
            }
            found = false;
            return first;
        }

        V* find(const K& k) {
            bool found;
            int j = _find(k, found);
            return found ? &values[j] : 0;
        }

    private:
        int n;
        K *keys;
        int *hashes;
        V *values;
    };

} // namespace mongo
