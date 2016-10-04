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

/** Unit tests for OwnedPointerMap. */

#include "mongo/base/owned_pointer_map.h"

#include <string>
#include <vector>

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using std::make_pair;
using std::string;

/** Helper class that logs its constructor argument to a static vector on destruction. */
class DestructionLogger {
public:
    DestructionLogger(const string& name) : _name(name) {}
    ~DestructionLogger() {
        _destroyedNames.push_back(_name);
    }

    static std::vector<string>& destroyedNames() {
        return _destroyedNames;
    }

    string getName() {
        return _name;
    }

private:
    string _name;
    static std::vector<string> _destroyedNames;
};

std::vector<string> DestructionLogger::_destroyedNames;

TEST(OwnedPointerMapTest, OwnedPointerDestroyed) {
    DestructionLogger::destroyedNames().clear();
    {
        OwnedPointerMap<int, DestructionLogger> owned;
        owned.mutableMap().insert(make_pair(0, new DestructionLogger("foo")));
        // owned destroyed
    }
    ASSERT_EQUALS(1U, DestructionLogger::destroyedNames().size());
    ASSERT_EQUALS("foo", DestructionLogger::destroyedNames()[0]);
}

TEST(OwnedPointerMapTest, OwnedConstPointerDestroyed) {
    DestructionLogger::destroyedNames().clear();
    {
        OwnedPointerMap<int, const DestructionLogger> owned;
        owned.mutableMap().insert(make_pair(0, new DestructionLogger("foo")));
        // owned destroyed
    }
    ASSERT_EQUALS(1U, DestructionLogger::destroyedNames().size());
    ASSERT_EQUALS("foo", DestructionLogger::destroyedNames()[0]);
}

TEST(OwnedPointerMapTest, OwnedPointersDestroyedInOrder) {
    DestructionLogger::destroyedNames().clear();
    {
        OwnedPointerMap<int, DestructionLogger> owned;
        owned.mutableMap().insert(make_pair(0, new DestructionLogger("first")));
        owned.mutableMap().insert(make_pair(1, new DestructionLogger("second")));
        // owned destroyed
    }
    ASSERT_EQUALS(2U, DestructionLogger::destroyedNames().size());
    ASSERT_EQUALS("first", DestructionLogger::destroyedNames()[0]);
    ASSERT_EQUALS("second", DestructionLogger::destroyedNames()[1]);
}

TEST(OwnedPointerMapTest, OwnedPointersWithCompare) {
    DestructionLogger::destroyedNames().clear();
    {
        OwnedPointerMap<int, DestructionLogger, std::greater<int>> owned;
        owned.mutableMap().insert(make_pair(0, new DestructionLogger("0")));
        owned.mutableMap().insert(make_pair(1, new DestructionLogger("1")));

        // use std::greater<int> rather than the default std::less<int>
        std::map<int, DestructionLogger*, std::greater<int>>::iterator it =
            owned.mutableMap().begin();

        ASSERT(owned.mutableMap().end() != it);
        // "1" should be sorted to be the first item.
        ASSERT_EQUALS("1", it->second->getName());

        it++;
        ASSERT(owned.mutableMap().end() != it);
        ASSERT_EQUALS("0", it->second->getName());

        // owned destroyed
    }
    // destroyed in descending order
    ASSERT_EQUALS(2U, DestructionLogger::destroyedNames().size());
    ASSERT_EQUALS("1", DestructionLogger::destroyedNames()[0]);
    ASSERT_EQUALS("0", DestructionLogger::destroyedNames()[1]);
}


}  // namespace
}  // namespace mongo
