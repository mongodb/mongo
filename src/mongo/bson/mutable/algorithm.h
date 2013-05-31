/* Copyright 2013 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstddef>
#include <algorithm>
#include <vector>

#include "mongo/bson/mutable/const_element.h"
#include "mongo/bson/mutable/element.h"

namespace mongo {
namespace mutablebson {

    /** For an overview of mutable BSON, please see the file document.h in this directory.
     *
     *  This file defines, in analogy with <algorithm>, a collection of useful algorithms for
     *  use with mutable BSON classes. In particular, algorithms for searching, sorting,
     *  indexed access, and counting are included.
     */

    /** 'findElement' searches rightward among the sibiling Elements of 'first', returning an
     *  Element representing the first item matching the predicate 'predicate'. If no Element
     *  matches, then the 'ok' method on the returned Element will return false.
     */
    template<typename ElementType, typename Predicate>
    inline ElementType findElement(ElementType first, Predicate predicate) {
        while (first.ok() && !predicate(first))
            first = first.rightSibling();
        return first;
    }

    /** A predicate for findElement that matches on the field name of Elements. */
    class FieldNameEquals {
    public:
        // The lifetime of this object must be a subset of the lifetime of 'fieldName'.
        explicit FieldNameEquals(const StringData& fieldName)
            : _fieldName(fieldName) {}

        bool operator()(const ConstElement& element) const {
            return (_fieldName == element.getFieldName());
        }

    private:
        const StringData& _fieldName;
    };

    /** A convenience wrapper around findElement<ElementType, FieldNameEquals>. */
    template<typename ElementType>
    inline ElementType findElementNamed(ElementType first, const StringData& fieldName) {
        return findElement(first, FieldNameEquals(fieldName));
    }

    /** Finds the first child under 'parent' that matches the given field name. If no such child
     *  Element is found, the returned Element's 'ok' method will return false.
     */
    template<typename ElementType>
    inline ElementType findFirstChildNamed(ElementType parent, const StringData& fieldName) {
        return findElementNamed(parent.leftChild(), fieldName);
    }

    /** A less-than ordering for Elements that compares based on the Element field names. */
    class FieldNameLessThan {
        // TODO: This should possibly derive from std::binary_function.
    public:
        inline bool operator()(const ConstElement& left, const ConstElement& right) const {
            return left.getFieldName() < right.getFieldName();
        }
    };

    /** Sort any children of Element 'parent' by way of Comparator 'comp', which should provide
     *  an operator() that takes two const Element&'s and implements a strict weak ordering.
     */
    template<typename Comparator>
    void sortChildren(Element parent, Comparator comp)  {
        // TODO: The following works, but obviously is not ideal.

        // First, build a vector of the children.
        std::vector<Element> children;
        Element current = parent.leftChild();
        while (current.ok()) {
            children.push_back(current);
            current = current.rightSibling();
        }

        // Then, sort the child vector with our comparator.
        std::sort(children.begin(), children.end(), comp);

        // Finally, reorder the children of parent according to the ordering established in
        // 'children'.
        std::vector<Element>::iterator where = children.begin();
        const std::vector<Element>::iterator end = children.end();
        for( ; where != end; ++where ) {
            // Detach from its current location.
            where->remove();
            // Make it the new rightmost element.
            parent.pushBack(*where);
        }
    }

    /** Remove any consecutive children that compare as identical according to 'comp'. The
     *  children must be sorted (see sortChildren, above), and the equality comparator here
     *  must be compatible with the comparator used for the sort.
     */
    template<typename EqualityComparator>
    void deduplicateChildren(Element parent, EqualityComparator equal) {
        Element current = parent.leftChild();
        while (current.ok()) {
            Element next = current.rightSibling();
            if (next.ok() && equal(current, next)) {
                next.remove();
            } else {
                current = next;
            }
        }
    }

    /** A less-than ordering for Elements that compares based on woCompare */
    class woLess {
        // TODO: This should possibly derive from std::binary_function.
    public:
        woLess(bool considerFieldName = true)
            : _considerFieldName(considerFieldName) {
        }

        inline bool operator()(const ConstElement& left, const ConstElement& right) const {
            return left.compareWithElement(right, _considerFieldName) < 0;
        }
    private:
        const bool _considerFieldName;
    };

    /** A greater-than ordering for Elements that compares based on woCompare */
    class woGreater {
        // TODO: This should possibly derive from std::binary_function.
    public:
        woGreater(bool considerFieldName = true)
            : _considerFieldName(considerFieldName) {
        }

        inline bool operator()(const ConstElement& left, const ConstElement& right) const {
            return left.compareWithElement(right, _considerFieldName) > 0;
        }
    private:
        const bool _considerFieldName;
    };

    /** An equality predicate for elements that compares based on woCompare */
    class woEqual {
        // TODO: This should possibly derive from std::binary_function.
    public:
        woEqual(bool considerFieldName = true)
            : _considerFieldName(considerFieldName) {
        }

        inline bool operator()(const ConstElement& left, const ConstElement& right) const {
            return left.compareWithElement(right, _considerFieldName) == 0;
        }
    private:
        const bool _considerFieldName;
    };

    /** An equality predicate for elements that compares based on woCompare */
    class woEqualTo {
        // TODO: This should possibly derive from std::binary_function.
    public:
        woEqualTo(const ConstElement& value, bool considerFieldName = true)
            : _value(value)
            , _considerFieldName(considerFieldName) {
        }

        inline bool operator()(const ConstElement& elt) const {
            return _value.compareWithElement(elt, _considerFieldName) == 0;
        }
    private:
        const ConstElement& _value;
        const bool _considerFieldName;
    };

    /** Return the element that is 'n' Elements to the left in the sibling chain of 'element'. */
    template<typename ElementType>
    ElementType getNthLeftSibling(ElementType element, std::size_t n) {
        while (element.ok() && (n-- != 0))
            element = element.leftSibling();
        return element;
    }

    /** Return the element that is 'n' Elements to the right in the sibling chain of 'element'. */
    template<typename ElementType>
    ElementType getNthRightSibling(ElementType element, std::size_t n) {
        while (element.ok() && (n-- != 0))
            element = element.rightSibling();
        return element;
    }

    /** Move 'n' Elements left or right in the sibling chain of 'element' */
    template<typename ElementType>
    ElementType getNthSibling(ElementType element, int n) {
        return (n < 0) ?
            getNthLeftSibling(element, -n) :
            getNthRightSibling(element, n);
    }

    /** Get the child that is 'n' Elements to the right of 'element's left child. */
    template<typename ElementType>
    ElementType getNthChild(ElementType element, std::size_t n) {
        return getNthRightSibling(element.leftChild(), n);
    }

    /** Returns the number of valid siblings to the left of 'element'. */
    template<typename ElementType>
    std::size_t countSiblingsLeft(ElementType element) {
        std::size_t result = 0;
        element = element.leftSibling();
        while (element.ok()) {
            element = element.leftSibling();
            ++result;
        }
        return result;
    }

    /** Returns the number of valid siblings to the right of 'element'. */
    template<typename ElementType>
    std::size_t countSiblingsRight(ElementType element) {
        std::size_t result = 0;
        element = element.rightSibling();
        while (element.ok()) {
            element = element.rightSibling();
            ++result;
        }
        return result;
    }

    /** Return the number of children of 'element'. */
    template<typename ElementType>
    std::size_t countChildren(ElementType element) {
        element = element.leftChild();
        return element.ok() ? (1 + countSiblingsRight(element)) : 0;
    }

} // namespace mutablebson
} // namespace mongo
