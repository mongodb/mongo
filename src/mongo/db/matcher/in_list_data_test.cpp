/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/matcher/in_list_data.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <vector>

namespace mongo {
namespace {
// Returns true if 'aElements' and 'bElements' contain the same BSONElements by the referred
// structure.
bool equals(const std::vector<BSONElement>& aElements, const std::vector<BSONElement>& bElements) {
    if (aElements.size() != bElements.size()) {
        return false;
    }
    for (std::size_t index = 0; index < aElements.size(); ++index) {
        if (aElements[index].rawdata() != bElements[index].rawdata()) {
            return false;
        }
    }
    return true;
}

// Verifies that 'inListElements' method 'getFirstOfEachType(getSortedAndDeduped)' method returns
// elements referred to by 'inListElements' with indices of elements specified by
// 'expectedElementIndices'. 'getSortedAndDeduped' is the parameter 'getFirstOfEachType()' method.
void assertFirstOfEachTypeReturnsReferredElements(
    const InListData& inListElements,
    const std::vector<std::size_t>& expectedElementIndices,
    bool getSortedAndDeduped = false) {
    // Determine expected elements.
    const auto& ownedElements = inListElements.getElements(false);
    std::vector<BSONElement> expectedGetFirstOfEachTypeReturnValue;
    for (auto index : expectedElementIndices) {
        expectedGetFirstOfEachTypeReturnValue.push_back(ownedElements[index]);
    }

    const auto firstOfEachTypeElements = inListElements.getFirstOfEachType(getSortedAndDeduped);

    // Build a string representation of the result for diagnostic purposes.
    std::string firstOfEachTypeElementsAsString;
    for (auto&& element : firstOfEachTypeElements) {
        firstOfEachTypeElementsAsString += element.toString();
        firstOfEachTypeElementsAsString += ", ";
    };

    ASSERT_TRUE(equals(firstOfEachTypeElements, expectedGetFirstOfEachTypeReturnValue))
        << "actual elements:" << firstOfEachTypeElementsAsString;
}

// Verifies 'getFirstOfEachType(false)' behavior on InListData state transitions.
TEST(InListData, GetFirstOfEachTypeOnStateTransitions) {
    InListData inListElements;
    auto elementArrayObj = BSON("attr" << BSON_ARRAY("a" << "b" << 1 << 2));
    auto elementArray = elementArrayObj["attr"].Obj();
    std::vector<BSONElement> elements;
    elementArray.elems(elements);

    // Transition 'inListElements' to the unowned elements state.
    ASSERT_OK(inListElements.setElementsArray(elementArray));
    ASSERT_TRUE(equals(inListElements.getFirstOfEachType(false), {elements[0], elements[2]}));

    // Transition to the owned elements state.
    inListElements.makeBSONOwned();

    // The first of each BSON type elements should not point to the same backing BSON.
    ASSERT_FALSE(equals(inListElements.getFirstOfEachType(false), {elements[0], elements[2]}));

    // The first of each BSON type elements should be from the elements set.
    assertFirstOfEachTypeReturnsReferredElements(inListElements, {0, 2});

    // Transition to the mixed ownership state - replace some elements.
    ASSERT_OK(inListElements.setElements({elements[3], inListElements.getElements(false)[1]}));
    assertFirstOfEachTypeReturnsReferredElements(inListElements, {0, 1});

    // Verify that a clone in the mixed ownership state state is correct.
    assertFirstOfEachTypeReturnsReferredElements(*inListElements.clone(), {0, 1});

    // Transition to the owned elements state.
    inListElements.makeBSONOwned();
    assertFirstOfEachTypeReturnsReferredElements(inListElements, {0, 1});

    // Verify that a clone in the owned elements state is correct.
    assertFirstOfEachTypeReturnsReferredElements(*inListElements.clone(), {0, 1});

    // Transition to the unowned elements state.
    ASSERT_OK(inListElements.setElementsArray(elementArray));
    assertFirstOfEachTypeReturnsReferredElements(inListElements, {0, 2});

    // Verify that a clone in the unowned elements state is correct.
    assertFirstOfEachTypeReturnsReferredElements(*inListElements.clone(), {0, 2});

    // Transition to the mixed ownership state - replace some elements.
    ASSERT_OK(inListElements.setElements({elements[3]}));
    assertFirstOfEachTypeReturnsReferredElements(inListElements, {0});

    // Transition to the unowned elements state.
    ASSERT_OK(inListElements.setElementsArray(elementArray));
    assertFirstOfEachTypeReturnsReferredElements(inListElements, {0, 2});

    {
        InListData inListElements;

        // Verify that uninitialized object has no elements.
        assertFirstOfEachTypeReturnsReferredElements(inListElements, {});

        // Transition to the mixed ownership state - replace some elements.
        ASSERT_OK(inListElements.setElements({elements[3]}));
        assertFirstOfEachTypeReturnsReferredElements(inListElements, {0});
    }
}

// Tests 'getFirstOfEachType(true)' behavior.
TEST(InListData, GetFirstOfEachTypeSortedAndDeduped) {
    InListData inListElements;

    auto elementArrayObj = BSON("attr" << BSON_ARRAY("b" << "a" << 2 << 1));
    const auto elementArray = elementArrayObj["attr"].Obj();
    ASSERT_OK(inListElements.setElementsArray(elementArray));
    const bool kGetSortedAndDeduped = true;

    // The first of each BSON type elements should be from the elements set.
    assertFirstOfEachTypeReturnsReferredElements(inListElements, {3, 1}, kGetSortedAndDeduped);

    // Verify that correct results are returned when the element list is sorted and deduplicated.
    auto objWithSortedElements = BSON("attr" << BSON_ARRAY("a" << "b"
                                                               << "c"));
    ASSERT_OK(inListElements.setElementsArray(objWithSortedElements["attr"].Obj()));
    assertFirstOfEachTypeReturnsReferredElements(inListElements, {0}, kGetSortedAndDeduped);
}
}  // namespace
}  // namespace mongo
