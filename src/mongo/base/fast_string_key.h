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

#include <string>
#include <boost/functional/hash.hpp>

#include "mongo/base/string_data.h"

namespace mongo {

    /** This class should be a mostly drop-in replacement for std::string as keys in maps and sets.
     *  The main advantage over std::string is that you can do lookups using c-strings without
     *  constructing a std::string and copying the query to a heap-allocated buffer.
     *
     *  NOTE: This is not a general purpose class. You should never create variables of this type.
     *
     *  WARNING: this class does not support string with embedded NUL bytes. It is most useful for
     *  holding the equivalent of BSONObj fieldNames which do not support NUL either.
     */
    class FastStringKey {
    public:
        /*implicit*/ FastStringKey(const char* s) : _str(s) {}
        /*implicit*/ FastStringKey(const std::string& s) : _str(s.c_str()) {}
        /*implicit*/ FastStringKey(const StringData& s) : _str(s.data()) {}

        /** This should only be called by containers when adding a new element
         *
         *  This class assumes that lookups in containers don't copy the query object. This isn't
         *  guaranteed by the standard, but based on the required signatures no sane implementation
         *  would. If some STL implementation did copy the query object, we'd be no worse off than
         *  with just using std::string.
         */
        FastStringKey(const FastStringKey& source) {
            if (source._holder)
                _holder = source._holder;
            else
                _holder = boost::shared_ptr<char>(strdup(source._str), free);

            _str = _holder.get();
        }


        const char* c_str() const { return _str; }

        bool operator <  (const FastStringKey& rhs) const { return strcmp(_str, rhs._str) <  0; }
        bool operator == (const FastStringKey& rhs) const { return strcmp(_str, rhs._str) == 0; }

        /** This class is compliant with the TR1 std::hash interface.
         *  To use it, use unordered_set<FastStringKey, FastStringKey::hash>.
         *  We can't specialize std::hash easily because it may be called std::tr1::hash
         */
        class hash {
        public:
            size_t operator() (const FastStringKey& str) const {
                const char* begin = str.c_str();
                const char* end = begin + strlen(begin);

                // TODO maybe consider alternate hashing algorithms
                return boost::hash_range(begin, end);
            }
        };

    protected: // only subclass is in fast_string_key_test.cpp
        bool isOwned() const { return _holder != NULL; }

    private:
        const char* _str;
        boost::shared_ptr<char> _holder; // NULL if not copied

        void operator= (const FastStringKey&); // not defined
    };
}
