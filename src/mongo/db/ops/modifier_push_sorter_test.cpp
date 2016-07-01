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

#include "mongo/db/ops/modifier_push_sorter.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

namespace {

using mongo::BSONObj;
using mongo::CollatorInterface;
using mongo::CollatorInterfaceMock;
using mongo::fromjson;
using mongo::PatternElementCmp;
using mongo::mutablebson::Document;
using mongo::mutablebson::Element;
using mongo::mutablebson::sortChildren;

class ObjectArray : public mongo::unittest::Test {
public:
    ObjectArray() : _doc(), _size(0) {}

    virtual void setUp() {
        Element arr = _doc.makeElementArray("x");
        ASSERT_TRUE(arr.ok());
        ASSERT_OK(_doc.root().pushBack(arr));
    }

    void addObj(BSONObj obj) {
        ASSERT_LESS_THAN_OR_EQUALS(_size, 3u);
        _objs[_size] = obj;
        _size++;

        ASSERT_OK(_doc.root()["x"].appendObject(mongo::StringData(), obj));
    }

    BSONObj getOrigObj(size_t i) {
        return _objs[i];
    }

    BSONObj getSortedObj(size_t i) {
        return getArray()[i].getValueObject();
    }

    Element getArray() {
        return _doc.root()["x"];
    }

private:
    Document _doc;
    BSONObj _objs[3];
    size_t _size;
};

TEST_F(ObjectArray, NormalOrder) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{b:1, a:1}"));
    addObj(fromjson("{a:3, b:2}"));
    addObj(fromjson("{b:3, a:2}"));

    sortChildren(getArray(), PatternElementCmp(fromjson("{'a':1,'b':1}"), collator));

    ASSERT_EQUALS(getOrigObj(0), getSortedObj(0));
    ASSERT_EQUALS(getOrigObj(1), getSortedObj(2));
    ASSERT_EQUALS(getOrigObj(2), getSortedObj(1));
}

TEST_F(ObjectArray, MixedOrder) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{b:1, a:1}"));
    addObj(fromjson("{a:3, b:2}"));
    addObj(fromjson("{b:3, a:2}"));

    sortChildren(getArray(), PatternElementCmp(fromjson("{b:1,a:-1}"), collator));

    ASSERT_EQUALS(getOrigObj(0), getSortedObj(0));
    ASSERT_EQUALS(getOrigObj(1), getSortedObj(1));
    ASSERT_EQUALS(getOrigObj(2), getSortedObj(2));
}

TEST_F(ObjectArray, ExtraFields) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{b:1, c:2, a:1}"));
    addObj(fromjson("{c:1, a:3, b:2}"));
    addObj(fromjson("{b:3, a:2}"));

    sortChildren(getArray(), PatternElementCmp(fromjson("{a:1,b:1}"), collator));

    ASSERT_EQUALS(getOrigObj(0), getSortedObj(0));
    ASSERT_EQUALS(getOrigObj(1), getSortedObj(2));
    ASSERT_EQUALS(getOrigObj(2), getSortedObj(1));
}

TEST_F(ObjectArray, MissingFields) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{a:2, b:2}"));
    addObj(fromjson("{a:1}"));
    addObj(fromjson("{a:3, b:3, c:3}"));

    sortChildren(getArray(), PatternElementCmp(fromjson("{b:1,c:1}"), collator));

    ASSERT_EQUALS(getOrigObj(0), getSortedObj(1));
    ASSERT_EQUALS(getOrigObj(1), getSortedObj(0));
    ASSERT_EQUALS(getOrigObj(2), getSortedObj(2));
}

TEST_F(ObjectArray, NestedFields) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{a:{b:{c:2, d:0}}}"));
    addObj(fromjson("{a:{b:{c:1, d:2}}}"));
    addObj(fromjson("{a:{b:{c:3, d:1}}}"));

    sortChildren(getArray(), PatternElementCmp(fromjson("{'a.b':1}"), collator));

    ASSERT_EQUALS(getOrigObj(0), getSortedObj(1));
    ASSERT_EQUALS(getOrigObj(1), getSortedObj(0));
    ASSERT_EQUALS(getOrigObj(2), getSortedObj(2));
}

TEST_F(ObjectArray, SimpleNestedFields) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{a:{b: -1}}"));
    addObj(fromjson("{a:{b: -100}}"));
    addObj(fromjson("{a:{b: 34}}"));

    sortChildren(getArray(), PatternElementCmp(fromjson("{'a.b':1}"), collator));

    ASSERT_EQUALS(getOrigObj(0), getSortedObj(1));
    ASSERT_EQUALS(getOrigObj(1), getSortedObj(0));
    ASSERT_EQUALS(getOrigObj(2), getSortedObj(2));
}

TEST_F(ObjectArray, NestedInnerObjectDescending) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{a:{b:{c:2, d:0}}}"));
    addObj(fromjson("{a:{b:{c:1, d:2}}}"));
    addObj(fromjson("{a:{b:{c:3, d:1}}}"));

    sortChildren(getArray(), PatternElementCmp(fromjson("{'a.b.d':-1}"), collator));

    ASSERT_EQUALS(getOrigObj(0), getSortedObj(2));
    ASSERT_EQUALS(getOrigObj(1), getSortedObj(0));
    ASSERT_EQUALS(getOrigObj(2), getSortedObj(1));
}

TEST_F(ObjectArray, NestedInnerObjectAscending) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{a:{b:{c:2, d:0}}}"));
    addObj(fromjson("{a:{b:{c:1, d:2}}}"));
    addObj(fromjson("{a:{b:{c:3, d:1}}}"));

    sortChildren(getArray(), PatternElementCmp(fromjson("{'a.b.d':1}"), collator));

    ASSERT_EQUALS(getOrigObj(0), getSortedObj(0));
    ASSERT_EQUALS(getOrigObj(2), getSortedObj(1));
    ASSERT_EQUALS(getOrigObj(1), getSortedObj(2));
}

TEST_F(ObjectArray, SortRespectsCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addObj(fromjson("{a: 'abg'}"));
    addObj(fromjson("{a: 'aca'}"));
    addObj(fromjson("{a: 'adc'}"));

    sortChildren(getArray(), PatternElementCmp(fromjson("{a: 1}"), &collator));

    ASSERT_EQUALS(getOrigObj(0), getSortedObj(2));
    ASSERT_EQUALS(getOrigObj(1), getSortedObj(0));
    ASSERT_EQUALS(getOrigObj(2), getSortedObj(1));
}

}  // unnamed namespace
