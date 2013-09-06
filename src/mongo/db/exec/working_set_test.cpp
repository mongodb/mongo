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

#include "mongo/db/exec/working_set.h"
#include "mongo/db/json.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

using namespace mongo;

namespace {

    class WorkingSetFixture : public mongo::unittest::Test {
    protected:
        void setUp() {
            WorkingSetID id = ws.allocate();
            ASSERT(id != WorkingSet::INVALID_ID);
            member = ws.get(id);
            ASSERT(NULL != member);
        }

        void tearDown() {
            ws = WorkingSet();
            member = NULL;
        }

        WorkingSet ws;
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
        member->obj = BSONObj(obj.objdata());
        ASSERT_TRUE(obj.isOwned());
        ASSERT_FALSE(member->obj.isOwned());

        // Get out the field we put in.
        BSONElement elt;
        ASSERT_TRUE(member->getFieldDotted(fieldName, &elt));
        ASSERT_EQUALS(elt.numberInt(), 5);
    }

    TEST_F(WorkingSetFixture, getFieldOwned) {
        string fieldName = "x";

        BSONObj obj = BSON(fieldName << 5);
        member->obj = obj;
        ASSERT_TRUE(member->obj.isOwned());
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

}  // namespace
