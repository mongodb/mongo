/* Copyright 2013 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/mutable/mutable_bson_test_utils.h"

#include <algorithm>
#include <ostream>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/const_element.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace mutablebson {

namespace {

inline void assertSameSign(int lhs, int rhs) {
    if (lhs == 0) {
        ASSERT_EQUALS(rhs, 0);
    } else if (lhs < 0) {
        ASSERT_LESS_THAN(rhs, 0);
    } else {
        ASSERT_GREATER_THAN(rhs, 0);
    }
}

inline void assertOppositeSign(int lhs, int rhs) {
    if (lhs == 0) {
        ASSERT_EQUALS(rhs, 0);
    } else if (lhs < 0) {
        ASSERT_GREATER_THAN(rhs, 0);
    } else {
        ASSERT_LESS_THAN(rhs, 0);
    }
}

void addChildrenToVector(ConstElement elt, std::vector<ConstElement>* accumulator) {
    ConstElement current = elt.leftChild();
    while (current.ok()) {
        accumulator->push_back(current);
        current = current.rightSibling();
    }
}

bool checkDocNoOrderingImpl(ConstElement lhs, ConstElement rhs) {
    const BSONType lhsType = lhs.getType();
    const BSONType rhsType = rhs.getType();

    if (lhsType == mongo::Object) {
        if (rhsType != mongo::Object)
            return false;

        // For objects, sort the children by field name, then compare in that order.

        std::vector<ConstElement> lhsChildren;
        addChildrenToVector(lhs, &lhsChildren);
        std::vector<ConstElement> rhsChildren;
        addChildrenToVector(rhs, &rhsChildren);
        if (lhsChildren.size() != rhsChildren.size())
            return false;

        // NOTE: if you have repeated field names, this is not necessarily going to
        // work. This is unlikely to be a problem in practice, but we could write a
        // more sophisticated comparator if we need to: perhaps one that ordered first
        // by field name, then by type, then by woCompare. Performance isn't important
        // here.
        std::sort(lhsChildren.begin(), lhsChildren.end(), FieldNameLessThan());
        std::sort(rhsChildren.begin(), rhsChildren.end(), FieldNameLessThan());

        typedef std::vector<ConstElement>::const_iterator iter;
        iter lhsWhere = lhsChildren.begin();
        iter rhsWhere = rhsChildren.begin();
        const iter lhsEnd = lhsChildren.end();

        for (; lhsWhere != lhsEnd; ++lhsWhere, ++rhsWhere) {
            if (lhsWhere->getFieldName() != rhsWhere->getFieldName())
                return false;

            if (!checkDocNoOrderingImpl(*lhsWhere, *rhsWhere))
                return false;
        }

        return true;

    } else if (lhsType == mongo::Array) {
        if (rhsType != mongo::Array)
            return false;

        // For arrays, since they are ordered, we don't need the sorting step.
        const size_t lhsChildren = countChildren(lhs);
        const size_t rhsChildren = countChildren(rhs);

        if (lhsChildren != rhsChildren)
            return false;

        if (lhsChildren == 0)
            return true;

        ConstElement lhsChild = lhs.leftChild();
        ConstElement rhsChild = rhs.leftChild();

        while (lhsChild.ok()) {
            if (!checkDocNoOrderingImpl(lhsChild, rhsChild))
                return false;

            lhsChild = lhsChild.rightSibling();
            rhsChild = rhsChild.rightSibling();
        }

        return true;

    } else {
        // This is some leaf type. We've already checked or ignored field names, so
        // don't recheck it here.
        return lhs.compareWithElement(rhs, false) == 0;
    }
}

}  // namespace

// TODO: We should really update this to be an ASSERT_ something, so that we can print out
// the expected and actual documents.
bool checkDoc(const Document& lhs, const BSONObj& rhs) {
    // Get the fundamental result via BSONObj's woCompare path. This is the best starting
    // point, because we think that Document::getObject and the serialization mechanism is
    // pretty well sorted.
    BSONObj fromLhs = lhs.getObject();
    const int primaryResult = fromLhs.woCompare(rhs);

    // Validate primary result via other comparison paths.
    const int secondaryResult = lhs.compareWithBSONObj(rhs);

    assertSameSign(primaryResult, secondaryResult);

    // Check that mutables serialized result matches against its origin.
    ASSERT_EQUALS(0, lhs.compareWithBSONObj(fromLhs));

    return (primaryResult == 0);
}

bool checkDoc(const Document& lhs, const Document& rhs) {
    const int primaryResult = lhs.compareWith(rhs);

    const BSONObj fromLhs = lhs.getObject();
    const BSONObj fromRhs = rhs.getObject();

    const int result_d_o = lhs.compareWithBSONObj(fromRhs);
    const int result_o_d = rhs.compareWithBSONObj(fromLhs);

    assertSameSign(primaryResult, result_d_o);
    assertOppositeSign(primaryResult, result_o_d);

    ASSERT_EQUALS(0, lhs.compareWithBSONObj(fromLhs));
    ASSERT_EQUALS(0, rhs.compareWithBSONObj(fromRhs));

    return (primaryResult == 0);
}

std::ostream& operator<<(std::ostream& stream, const ConstElement& elt) {
    stream << elt.toString();
    return stream;
}

std::ostream& operator<<(std::ostream& stream, const Document& doc) {
    stream << doc.toString();
    return stream;
}

std::ostream& operator<<(std::ostream& stream, const Element& elt) {
    stream << elt.toString();
    return stream;
}

bool checkEqualNoOrdering(const Document& lhs, const Document& rhs) {
    return checkDocNoOrderingImpl(lhs.root(), rhs.root());
}

std::ostream& operator<<(std::ostream& stream, const UnorderedWrapper_Doc& uw_d) {
    return stream << uw_d.doc;
}

std::ostream& operator<<(std::ostream& stream, const UnorderedWrapper_Obj& uw_o) {
    const Document d(uw_o.obj);
    return stream << d;
}

}  // namespace mutablebson
}  // namespace mongo
