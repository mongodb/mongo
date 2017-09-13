/*    Copyright 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include "mongo/util/unordered_fast_key_table.h"

namespace mongo {

template <typename Key, typename Hasher>
struct UnorderedFastKeyTableTraitsFactoryForPtrKey {
    struct Traits {
        static uint32_t hash(const Key* a) {
            return Hasher()(*a);
        }

        static bool equals(const Key* a, const Key* b) {
            return *a == *b;
        }

        static Key toStorage(const Key* s) {
            return *s;
        }

        static const Key* toLookup(const Key& s) {
            return &s;
        }

        class HashedKey {
        public:
            HashedKey() = default;

            explicit HashedKey(const Key* key) : _key(key), _hash(Traits::hash(_key)) {}

            HashedKey(const Key* key, uint32_t hash) : _key(key), _hash(hash) {
                // If you claim to know the hash, it better be correct.
                dassert(_hash == Traits::hash(_key));
            }

            const Key* key() const {
                return _key;
            }

            uint32_t hash() const {
                return _hash;
            }

        private:
            const Key* _key = nullptr;
            uint32_t _hash = 0;
        };
    };

    template <typename V>
    using type = UnorderedFastKeyTable<Key*, Key, V, Traits>;
};

/**
 * Provides a Hasher which forwards to an instance's .hash() method.  This should only be used with
 * high quality hashing functions because UnorderedFastKeyMap uses bit masks, rather than % by
 * prime, which can provide poor behavior without good overall distribution.
 */
template <typename T>
struct UnorderedFastKeyTableInstanceMethodHasher {
    auto operator()(const T& t) const -> decltype(t.hash()) {
        return t.hash();
    }
};
}
