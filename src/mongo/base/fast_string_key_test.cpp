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

/**
 * Unit tests of the FastStringKey type.
 */

#include <mongo/pch.h> // needed for mongoutils str.h

#include "mongo/base/fast_string_key.h"

#include <map>
#include <set>
#include <boost/unordered_map.hpp>
#include <boost/unordered_set.hpp>

#include "mongo/platform/unordered_map.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/unittest/unittest.h"

namespace {
    using namespace std;
    using namespace mongo;
    using namespace mongoutils;

    // This does extra checking that the map/set types use this class as expected
    class FastStringKeyStrict : public FastStringKey {
    public:
        // can't use "using" on constructor
        /*implicit*/ FastStringKeyStrict(const char* s) : FastStringKey(s) {}
        /*implicit*/ FastStringKeyStrict(const std::string& s) : FastStringKey(s) {}
        /*implicit*/ FastStringKeyStrict(const StringData& s) : FastStringKey(s) {}

        FastStringKeyStrict(const FastStringKeyStrict& s) : FastStringKey(s) {
            ASSERT_TRUE(allow_FastStringKey_copies);
        }

        bool operator < (const FastStringKeyStrict& rhs) const {
            // at least one operand should be owned since it is in the map/set
            ASSERT_TRUE(this->isOwned() || rhs.isOwned());

            return FastStringKey::operator<(rhs);
        }
        bool operator == (const FastStringKeyStrict& rhs) const {
            // at least one operand should be owned since it is in the map/set
            ASSERT_TRUE(this->isOwned() || rhs.isOwned());

            return FastStringKey::operator==(rhs);
        }

        friend class DisallowCopies;
        class DisallowCopies {
        public:
            DisallowCopies()  { FastStringKeyStrict::allow_FastStringKey_copies = false; }
            ~DisallowCopies() { FastStringKeyStrict::allow_FastStringKey_copies = true; }
        };

    private:
        void assertOneOwned(const FastStringKeyStrict& rhs) const {
        }

        static bool allow_FastStringKey_copies;
    };
    bool FastStringKeyStrict::allow_FastStringKey_copies = true;

    template <typename Map>
    void fillMap(Map& m) {
        m[""] = 1;
        m["a"] = 1;
        m["aa"] = 1;
        m["bb"] = 1;
        m[string("b")] = 1;
        m[StringData("abracadabra")] = 1;
    }

    template <typename Map>
    void fillMapPairs(Map& m) {
        m.insert(make_pair("", 1));
        m.insert(make_pair("a", 1));
        m.insert(make_pair("aa", 1));
        m.insert(make_pair("bb", 1));
        m.insert(make_pair(string("b"), 1));
        m.insert(make_pair(StringData("abracadabra"), 1));
    }

    template <typename Set>
    void fillSet(Set& s) {
        s.insert("");
        s.insert("a");
        s.insert("aa");
        s.insert("bb");
        s.insert(string("b"));
        s.insert(StringData("abracadabra"));
    }

    template <typename StringType, typename MapOrSet>
    void checkGeneric(const MapOrSet& ms, unsigned fills) {
        FastStringKeyStrict::DisallowCopies no_copying_while_reading;

        ASSERT_EQUALS(ms.size(), 6u * fills);

        ASSERT_EQUALS(ms.count(StringType("")), fills);
        ASSERT_EQUALS(ms.count(StringType("a")), fills);
        ASSERT_EQUALS(ms.count(StringType("aa")), fills);
        ASSERT_EQUALS(ms.count(StringType("bb")), fills);
        ASSERT_EQUALS(ms.count(StringType("b")), fills);
        ASSERT_EQUALS(ms.count(StringType("abracadabra")), fills);

        ASSERT_EQUALS(ms.count(StringType("no such key")), 0u);
    }

    // Makes the TEMPLATE_TEST_INSTANCEs below easier to read
    enum IsMulti{Single=false, Multi=true};

    template<typename ContainerType, void(*fillFunc)(ContainerType&), IsMulti isMulti>
    TEMPLATE_TEST(FastStringKeyTest, TestContainer) {
        ContainerType cont;

        fillFunc(cont);
        unsigned fills = 1;

        checkGeneric<const char*>(cont, fills);
        checkGeneric<string>(cont, fills);
        checkGeneric<StringData>(cont, fills);

        fillFunc(cont);
        if (isMulti) fills++;

        checkGeneric<const char*>(cont, isMulti?2:1);
        checkGeneric<string>(cont, isMulti?2:1);
        checkGeneric<StringData>(cont, isMulti?2:1);
    }
    // Map tests
    TEMPLATE_TEST_INSTANCE(FastStringKeyTest, TestContainer,
            map<FastStringKeyStrict, int>,
            fillMap, Single);
    TEMPLATE_TEST_INSTANCE(FastStringKeyTest, TestContainer,
            multimap<FastStringKeyStrict, int>,
            fillMapPairs, Multi);
    TEMPLATE_TEST_INSTANCE(FastStringKeyTest, TestContainer,
            unordered_map<FastStringKeyStrict, int, FastStringKey::hash>,
            fillMap, Single);
    TEMPLATE_TEST_INSTANCE(FastStringKeyTest, TestContainer,
            boost::unordered_map<FastStringKeyStrict, int, FastStringKey::hash>,
            fillMap, Single);
    TEMPLATE_TEST_INSTANCE(FastStringKeyTest, TestContainer,
            boost::unordered_multimap<FastStringKeyStrict, int, FastStringKey::hash>,
            fillMapPairs, Multi);

    // Set tests
    TEMPLATE_TEST_INSTANCE(FastStringKeyTest, TestContainer,
            set<FastStringKeyStrict>,
            fillSet, Single);
    TEMPLATE_TEST_INSTANCE(FastStringKeyTest, TestContainer,
            multiset<FastStringKeyStrict>,
            fillSet, Multi);
    TEMPLATE_TEST_INSTANCE(FastStringKeyTest, TestContainer,
            unordered_set<FastStringKeyStrict, FastStringKey::hash>,
            fillSet, Single);
    TEMPLATE_TEST_INSTANCE(FastStringKeyTest, TestContainer,
            boost::unordered_set<FastStringKeyStrict, FastStringKey::hash>,
            fillSet, Single);
    TEMPLATE_TEST_INSTANCE(FastStringKeyTest, TestContainer,
            boost::unordered_multiset<FastStringKeyStrict, FastStringKey::hash>,
            fillSet, Multi);

    TEST(FastStringKeyTest, HashUniqueness) {
        // Test all 0 and 1 char strings for hash uniqueness
        // This test isn't strictly guaranteed to pass, but if it doesn't we would want to know.
        // If it passes once, it will continue passing unless the hash function changes.

        unordered_map<size_t, string> hashes;

        for (char c='\0'; c < '\x7f'; c++) {
            const char str[] = {c, '\0'};
            const size_t hash = FastStringKey::hash()(str);

            if (hashes.count(hash)) {
                FAIL(str::stream() << "duplicate hash detected!"
                                   << " hash: " << hash
                                   << " str1: \"" << hashes[hash] << "\" " << int(hashes[hash][0])
                                   << " str2: \"" << str << "\" " << int(str[0])
                                   );
            }

            hashes[hash] = str;
        }
    }
}
