// string_map.h

/*    Copyright 2012 10gen Inc.
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

#include <third_party/murmurhash3/MurmurHash3.h>

#include "mongo/base/string_data.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/unordered_fast_key_table.h"

namespace mongo {

struct StringMapTraits {
    static uint32_t hash(StringData a) {
        uint32_t hash;
        MurmurHash3_x86_32(a.rawData(), a.size(), 0, &hash);
        return hash;
    }

    static bool equals(StringData a, StringData b) {
        return a == b;
    }

    static std::string toStorage(StringData s) {
        return s.toString();
    }

    static StringData toLookup(const std::string& s) {
        return StringData(s);
    }

    class HashedKey {
    public:
        explicit HashedKey(StringData key = "") : _key(key), _hash(StringMapTraits::hash(_key)) {}

        HashedKey(StringData key, uint32_t hash) : _key(key), _hash(hash) {
            // If you claim to know the hash, it better be correct.
            dassert(_hash == StringMapTraits::hash(_key));
        }

        const StringData& key() const {
            return _key;
        }

        uint32_t hash() const {
            return _hash;
        }

    private:
        StringData _key;
        uint32_t _hash;
    };
};

template <typename V>
using StringMap = UnorderedFastKeyTable<StringData,   // K_L
                                        std::string,  // K_S
                                        V,
                                        StringMapTraits>;

}  // namespace mongo
