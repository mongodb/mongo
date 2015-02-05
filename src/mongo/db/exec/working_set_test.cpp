/**
 *    Copyright (C) 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

/**
 * This file contains tests for mongo/db/exec/working_set.cpp
 */

#include <boost/scoped_ptr.hpp>

#include "mongo/db/exec/working_set.h"
#include "mongo/db/json.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

using namespace mongo;

namespace {

    using std::string;

    class WorkingSetFixture : public mongo::unittest::Test {
    protected:
        void setUp() {
            ws.reset(new WorkingSet());
            WorkingSetID id = ws->allocate();
            ASSERT(id != WorkingSet::INVALID_ID);
            member = ws->get(id);
            ASSERT(NULL != member);
        }

        void tearDown() {
            ws.reset();
            member = NULL;
        }

        boost::scoped_ptr<WorkingSet> ws;
        WorkingSetMember* member;
    };

    TEST_F(WorkingSetFixture, noFieldToGet) {
        BSONElement elt;

        // Make sure we're not getting anything out of an invalid WSM.
        ASSERT_EQUALS(WorkingSetMember::INVALID, member->state);
        ASSERT_FALSE(member->getFieldDotted("foo", &elt));

        member->state = WorkingSetMember::LOC_AND_IDX;
        ASSERT_FALSE(member->getFieldDotted("foo", &elt));

        // Our state is that of a valid object.  The getFieldDotted shouldn't throw; there's
        // something to call getFieldDotted on, but there's no field there.
        member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;
        ASSERT_TRUE(member->getFieldDotted("foo", &elt));

        member->state = WorkingSetMember::OWNED_OBJ;
        ASSERT_TRUE(member->getFieldDotted("foo", &elt));
    }

    TEST_F(WorkingSetFixture, getFieldUnowned) {
        string fieldName = "x";

        BSONObj obj = BSON(fieldName << 5);
        // Not truthful since the loc is bogus, but the loc isn't accessed anyway...
        member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;
        member->obj = Snapshotted<BSONObj>(SnapshotId(), BSONObj(obj.objdata()));
        ASSERT_TRUE(obj.isOwned());
        ASSERT_FALSE(member->obj.value().isOwned());

        // Get out the field we put in.
        BSONElement elt;
        ASSERT_TRUE(member->getFieldDotted(fieldName, &elt));
        ASSERT_EQUALS(elt.numberInt(), 5);
    }

    TEST_F(WorkingSetFixture, getFieldOwned) {
        string fieldName = "x";

        BSONObj obj = BSON(fieldName << 5);
        member->obj = Snapshotted<BSONObj>(SnapshotId(), obj);
        ASSERT_TRUE(member->obj.value().isOwned());
        member->state = WorkingSetMember::OWNED_OBJ;
        BSONElement elt;
        ASSERT_TRUE(member->getFieldDotted(fieldName, &elt));
        ASSERT_EQUALS(elt.numberInt(), 5);
    }

    TEST_F(WorkingSetFixture, getFieldFromIndex) {
        string firstName = "x";
        int firstValue = 5;

        string secondName = "y";
        int secondValue = 10;

        member->keyData.push_back(IndexKeyDatum(BSON(firstName << 1), BSON("" << firstValue)));
        // Also a minor lie as loc is bogus.
        member->state = WorkingSetMember::LOC_AND_IDX;
        BSONElement elt;
        ASSERT_TRUE(member->getFieldDotted(firstName, &elt));
        ASSERT_EQUALS(elt.numberInt(), firstValue);
        // No foo field.
        ASSERT_FALSE(member->getFieldDotted("foo", &elt));

        // Add another index datum.
        member->keyData.push_back(IndexKeyDatum(BSON(secondName << 1), BSON("" << secondValue)));
        ASSERT_TRUE(member->getFieldDotted(secondName, &elt));
        ASSERT_EQUALS(elt.numberInt(), secondValue);
        ASSERT_TRUE(member->getFieldDotted(firstName, &elt));
        ASSERT_EQUALS(elt.numberInt(), firstValue);
        // Still no foo.
        ASSERT_FALSE(member->getFieldDotted("foo", &elt));
    }

    TEST_F(WorkingSetFixture, getDottedFieldFromIndex) {
        string firstName = "x.y";
        int firstValue = 5;

        member->keyData.push_back(IndexKeyDatum(BSON(firstName << 1), BSON("" << firstValue)));
        member->state = WorkingSetMember::LOC_AND_IDX;
        BSONElement elt;
        ASSERT_TRUE(member->getFieldDotted(firstName, &elt));
        ASSERT_EQUALS(elt.numberInt(), firstValue);
        ASSERT_FALSE(member->getFieldDotted("x", &elt));
        ASSERT_FALSE(member->getFieldDotted("y", &elt));
    }

    //
    // WorkingSet::iterator tests
    //

    TEST(WorkingSetIteratorTest, BasicIteratorTest) {
        WorkingSet ws;

        WorkingSetID id1 = ws.allocate();
        WorkingSetMember* member1 = ws.get(id1);
        member1->state = WorkingSetMember::LOC_AND_IDX;
        member1->keyData.push_back(IndexKeyDatum(BSON("a" << 1), BSON("" << 3)));

        WorkingSetID id2 = ws.allocate();
        WorkingSetMember* member2 = ws.get(id2);
        member2->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;
        member2->obj = Snapshotted<BSONObj>(SnapshotId(), BSON("a" << 3));

        int counter = 0;
        for (WorkingSet::iterator it = ws.begin(); it != ws.end(); ++it) {
            ASSERT(it->state == WorkingSetMember::LOC_AND_IDX ||
                   it->state == WorkingSetMember::LOC_AND_UNOWNED_OBJ);
            counter++;
        }
        ASSERT_EQ(counter, 2);
    }

    TEST(WorkingSetIteratorTest, EmptyWorkingSet) {
        WorkingSet ws;

        int counter = 0;
        for (WorkingSet::iterator it = ws.begin(); it != ws.end(); ++it) {
            counter++;
        }
        ASSERT_EQ(counter, 0);
    }

    TEST(WorkingSetIteratorTest, EmptyWorkingSetDueToFree) {
        WorkingSet ws;

        WorkingSetID id = ws.allocate();
        ws.free(id);

        int counter = 0;
        for (WorkingSet::iterator it = ws.begin(); it != ws.end(); ++it) {
            counter++;
        }
        ASSERT_EQ(counter, 0);
    }

    TEST(WorkingSetIteratorTest, MixedFreeAndInUse) {
        WorkingSet ws;

        WorkingSetID id1 = ws.allocate();
        WorkingSetID id2 = ws.allocate();
        WorkingSetID id3 = ws.allocate();

        WorkingSetMember* member = ws.get(id2);
        member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;
        member->obj = Snapshotted<BSONObj>(SnapshotId(), BSON("a" << 3));

        ws.free(id1);
        ws.free(id3);

        int counter = 0;
        for (WorkingSet::iterator it = ws.begin(); it != ws.end(); ++it) {
            ASSERT(it->state == WorkingSetMember::LOC_AND_UNOWNED_OBJ);
            counter++;
        }
        ASSERT_EQ(counter, 1);
    }

    TEST(WorkingSetIteratorTest, FreeWhileIterating) {
        WorkingSet ws;

        ws.allocate();
        ws.allocate();
        ws.allocate();

        // Free the last two members during iteration.
        int counter = 0;
        for (WorkingSet::iterator it = ws.begin(); it != ws.end(); ++it) {
            if (counter > 0) {
                it.free();
            }
            counter++;
        }
        ASSERT_EQ(counter, 3);

        // Verify that only one item remains in the working set.
        counter = 0;
        for (WorkingSet::iterator it = ws.begin(); it != ws.end(); ++it) {
            counter++;
        }
        ASSERT_EQ(counter, 1);
    }

}  // namespace
