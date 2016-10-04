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

/** Unit tests for OwnedPointerVector. */

#include "mongo/base/owned_pointer_vector.h"

#include <string>
#include <vector>

#include "mongo/unittest/unittest.h"

namespace mongo {

using std::string;

namespace {

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

private:
    string _name;
    static std::vector<string> _destroyedNames;
};

std::vector<string> DestructionLogger::_destroyedNames;

TEST(OwnedPointerVectorTest, OwnedPointerDestroyed) {
    DestructionLogger::destroyedNames().clear();
    {
        OwnedPointerVector<DestructionLogger> owned;
        owned.mutableVector().push_back(new DestructionLogger("foo"));
        // owned destroyed
    }
    ASSERT_EQUALS(1U, DestructionLogger::destroyedNames().size());
    ASSERT_EQUALS("foo", DestructionLogger::destroyedNames()[0]);
}

TEST(OwnedPointerVectorTest, OwnedConstPointerDestroyed) {
    DestructionLogger::destroyedNames().clear();
    {
        OwnedPointerVector<const DestructionLogger> owned;
        owned.push_back(new DestructionLogger("foo"));
        // owned destroyed
    }
    ASSERT_EQUALS(1U, DestructionLogger::destroyedNames().size());
    ASSERT_EQUALS("foo", DestructionLogger::destroyedNames()[0]);
}

TEST(OwnedPointerVectorTest, OwnedPointersDestroyedInOrder) {
    DestructionLogger::destroyedNames().clear();
    {
        OwnedPointerVector<DestructionLogger> owned;
        owned.push_back(new DestructionLogger("first"));
        owned.push_back(new DestructionLogger("second"));
        // owned destroyed
    }
    ASSERT_EQUALS(2U, DestructionLogger::destroyedNames().size());
    ASSERT_EQUALS("first", DestructionLogger::destroyedNames()[0]);
    ASSERT_EQUALS("second", DestructionLogger::destroyedNames()[1]);
}

TEST(OwnedPointerVectorTest, ClearDestroyedInOrder) {
    DestructionLogger::destroyedNames().clear();
    {
        OwnedPointerVector<DestructionLogger> owned;
        owned.push_back(new DestructionLogger("first"));
        owned.push_back(new DestructionLogger("second"));

        owned.clear();
        ASSERT_EQUALS(2U, DestructionLogger::destroyedNames().size());
        ASSERT_EQUALS("first", DestructionLogger::destroyedNames()[0]);
        ASSERT_EQUALS("second", DestructionLogger::destroyedNames()[1]);
        ASSERT_EQUALS(0U, owned.size());
        // owned destroyed
    }
    // no additional deletion should have occured when owned was destroyed
    ASSERT_EQUALS(2U, DestructionLogger::destroyedNames().size());
}

TEST(OwnedPointerVectorTest, EraseDestroysAsCalled) {
    DestructionLogger::destroyedNames().clear();
    {
        // names are order of erasure
        OwnedPointerVector<DestructionLogger> owned;
        owned.push_back(new DestructionLogger("third"));
        owned.push_back(new DestructionLogger("first"));
        owned.push_back(new DestructionLogger("second"));
        owned.push_back(new DestructionLogger("fourth"));

        ASSERT_EQUALS(0U, DestructionLogger::destroyedNames().size());

        // erase "first", sliding "second" down to index 1
        owned.erase(owned.begin() + 1);
        ASSERT_EQUALS(1U, DestructionLogger::destroyedNames().size());
        ASSERT_EQUALS("first", DestructionLogger::destroyedNames().back());
        ASSERT_EQUALS(3U, owned.size());

        // erase "second"
        owned.erase(owned.begin() + 1);
        ASSERT_EQUALS(2U, DestructionLogger::destroyedNames().size());
        ASSERT_EQUALS("second", DestructionLogger::destroyedNames().back());
        ASSERT_EQUALS(2U, owned.size());

        // erase "third"
        owned.erase(owned.begin() + 0);
        ASSERT_EQUALS(3U, DestructionLogger::destroyedNames().size());
        ASSERT_EQUALS("third", DestructionLogger::destroyedNames().back());
        ASSERT_EQUALS(1U, owned.size());

        // owned destroyed
    }

    // only "four" should have been deleted when owned was destroyed
    ASSERT_EQUALS(4U, DestructionLogger::destroyedNames().size());
    ASSERT_EQUALS("fourth", DestructionLogger::destroyedNames().back());
}

TEST(OwnedPointerVectorTest, Accessors) {
    OwnedPointerVector<int> owned;
    ASSERT_TRUE(owned.empty());
    ASSERT_EQUALS(0U, owned.size());

    owned.push_back(new int(0));
    owned.push_back(new int(1));
    owned.push_back(new int(2));

    ASSERT_FALSE(owned.empty());
    ASSERT_EQUALS(3U, owned.size());

    ASSERT_EQUALS(0, *owned[0]);
    ASSERT_EQUALS(1, *owned[1]);
    ASSERT_EQUALS(2, *owned[2]);

    ASSERT_EQUALS(0, *owned.front());
    ASSERT_EQUALS(2, *owned.back());
}

TEST(OwnedPointerVectorTest, TransferConstructor) {
    DestructionLogger::destroyedNames().clear();
    {
        OwnedPointerVector<DestructionLogger> source;
        source.push_back(new DestructionLogger("first"));
        source.push_back(new DestructionLogger("second"));

        {
            OwnedPointerVector<DestructionLogger> dest(source.release());
            ASSERT_EQUALS(0U, DestructionLogger::destroyedNames().size());
            ASSERT_EQUALS(0U, source.size());
            ASSERT_EQUALS(2U, dest.size());
            // dest destroyed
        }
        ASSERT_EQUALS(2U, DestructionLogger::destroyedNames().size());
        ASSERT_EQUALS("first", DestructionLogger::destroyedNames()[0]);
        ASSERT_EQUALS("second", DestructionLogger::destroyedNames()[1]);

        // source destroyed
    }
    // no additional deletions
    ASSERT_EQUALS(2U, DestructionLogger::destroyedNames().size());
}

TEST(OwnedPointerVectorTest, TransferConstructorDoesntModifyArgument) {
    DestructionLogger::destroyedNames().clear();
    {
        std::vector<DestructionLogger*> source;
        source.push_back(new DestructionLogger("first"));
        source.push_back(new DestructionLogger("second"));

        {
            OwnedPointerVector<DestructionLogger> dest(source);
            ASSERT_EQUALS(0U, DestructionLogger::destroyedNames().size());
            ASSERT_EQUALS(2U, source.size());
            ASSERT_EQUALS(2U, dest.size());
            ASSERT(source == dest.vector());  // can't use ASSERT_EQUALS
            // dest destroyed
        }
        ASSERT_EQUALS(2U, DestructionLogger::destroyedNames().size());
        ASSERT_EQUALS("first", DestructionLogger::destroyedNames()[0]);
        ASSERT_EQUALS("second", DestructionLogger::destroyedNames()[1]);

        ASSERT_EQUALS(2U, source.size());
        // source destroyed
    }
    // no additional deletions
    ASSERT_EQUALS(2U, DestructionLogger::destroyedNames().size());
}

TEST(OwnedPointerVectorTest, TransferAssignment) {
    DestructionLogger::destroyedNames().clear();
    {
        OwnedPointerVector<DestructionLogger> dest;
        {
            OwnedPointerVector<DestructionLogger> source;
            source.push_back(new DestructionLogger("first"));
            source.push_back(new DestructionLogger("second"));

            dest = source.release();
            ASSERT_EQUALS(0U, DestructionLogger::destroyedNames().size());
            ASSERT_EQUALS(0U, source.size());
            ASSERT_EQUALS(2U, dest.size());
            // source destroyed
        }

        ASSERT_EQUALS(0U, DestructionLogger::destroyedNames().size());
        ASSERT_EQUALS(2U, dest.size());
        // dest destroyed
    }
    ASSERT_EQUALS(2U, DestructionLogger::destroyedNames().size());
    ASSERT_EQUALS("first", DestructionLogger::destroyedNames()[0]);
    ASSERT_EQUALS("second", DestructionLogger::destroyedNames()[1]);
}

TEST(OwnedPointerVectorTest, TransferAssignmentDoesntModifyArgument) {
    DestructionLogger::destroyedNames().clear();
    {
        OwnedPointerVector<DestructionLogger> dest;
        {
            std::vector<DestructionLogger*> source;
            source.push_back(new DestructionLogger("first"));
            source.push_back(new DestructionLogger("second"));

            dest = source;
            ASSERT_EQUALS(0U, DestructionLogger::destroyedNames().size());
            ASSERT_EQUALS(2U, source.size());
            ASSERT_EQUALS(2U, dest.size());
            ASSERT(source == dest.vector());  // can't use ASSERT_EQUALS
            // source destroyed
        }

        ASSERT_EQUALS(0U, DestructionLogger::destroyedNames().size());
        ASSERT_EQUALS(2U, dest.size());
        // dest destroyed
    }
    ASSERT_EQUALS(2U, DestructionLogger::destroyedNames().size());
    ASSERT_EQUALS("first", DestructionLogger::destroyedNames()[0]);
    ASSERT_EQUALS("second", DestructionLogger::destroyedNames()[1]);
}

TEST(OwnedPointerVectorTest, ReleaseAt) {
    DestructionLogger::destroyedNames().clear();

    std::unique_ptr<DestructionLogger> holder;
    {
        // names are order of deletion
        OwnedPointerVector<DestructionLogger> owned;
        owned.push_back(new DestructionLogger("first"));
        owned.push_back(new DestructionLogger("third"));
        owned.push_back(new DestructionLogger("second"));

        ASSERT_EQUALS(0U, DestructionLogger::destroyedNames().size());

        // transfer ownership of "third" to holder
        holder.reset(owned.releaseAt(1));
        ASSERT_EQUALS(0U, DestructionLogger::destroyedNames().size());
        ASSERT_EQUALS(3U, owned.size());
        ASSERT_EQUALS(static_cast<DestructionLogger*>(NULL), owned[1]);

        // owned destroyed
    }
    // owned deleted "first" and "second", but not "third"
    ASSERT_EQUALS(2U, DestructionLogger::destroyedNames().size());
    ASSERT_EQUALS("first", DestructionLogger::destroyedNames()[0]);
    ASSERT_EQUALS("second", DestructionLogger::destroyedNames()[1]);

    // delete "third"
    holder.reset();
    ASSERT_EQUALS(3U, DestructionLogger::destroyedNames().size());
    ASSERT_EQUALS("third", DestructionLogger::destroyedNames().back());
}

TEST(OwnedPointerVectorTest, PopAndReleaseBack) {
    DestructionLogger::destroyedNames().clear();

    {
        // names are order of deletion
        OwnedPointerVector<DestructionLogger> owned;
        owned.push_back(new DestructionLogger("second"));
        owned.push_back(new DestructionLogger("third"));
        owned.push_back(new DestructionLogger("first"));

        ASSERT_EQUALS(0U, DestructionLogger::destroyedNames().size());

        {
            // transfer ownership of "third" to holder
            std::unique_ptr<DestructionLogger> holder(owned.popAndReleaseBack());
            ASSERT_EQUALS(0U, DestructionLogger::destroyedNames().size());
            ASSERT_EQUALS(2U, owned.size());
            // holder destroyed
        }
        ASSERT_EQUALS(1U, DestructionLogger::destroyedNames().size());
        ASSERT_EQUALS("first", DestructionLogger::destroyedNames().back());
        // owned destroyed
    }
    // owned destructor deleted "second" and "third", but not "first"
    ASSERT_EQUALS(3U, DestructionLogger::destroyedNames().size());
    ASSERT_EQUALS("second", DestructionLogger::destroyedNames()[1]);
    ASSERT_EQUALS("third", DestructionLogger::destroyedNames()[2]);
}

TEST(OwnedPointerVectorTest, PopAndDeleteBack) {
    DestructionLogger::destroyedNames().clear();

    {
        // names are order of deletion
        OwnedPointerVector<DestructionLogger> owned;
        owned.push_back(new DestructionLogger("second"));
        owned.push_back(new DestructionLogger("third"));
        owned.push_back(new DestructionLogger("first"));

        ASSERT_EQUALS(0U, DestructionLogger::destroyedNames().size());

        owned.popAndDeleteBack();
        ASSERT_EQUALS(2U, owned.size());
        ASSERT_EQUALS(1U, DestructionLogger::destroyedNames().size());
        ASSERT_EQUALS("first", DestructionLogger::destroyedNames().back());
        // owned destroyed
    }
    // owned destructor deleted "second" and "third", but not "first"
    ASSERT_EQUALS(3U, DestructionLogger::destroyedNames().size());
    ASSERT_EQUALS("second", DestructionLogger::destroyedNames()[1]);
    ASSERT_EQUALS("third", DestructionLogger::destroyedNames()[2]);
}

}  // namespace
}  // namespace mongo
