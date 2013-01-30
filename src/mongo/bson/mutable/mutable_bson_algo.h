/* Copyright 2012 10gen Inc.
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

#include "mongo/bson/mutable/mutable_bson.h"

namespace mongo {
namespace mutablebson {

    /** Searches forward among the Elements iterated by 'first',
     *  returning an iterator to the first item matching the predicate
     *  'p'. If no element matches 'p', then the 'done' method on the
     *  returned iterator will return true. Note that this method
     *  takes SiblingIterators, so it does not search into
     *  subdocuments, only within siblings.
     */
    template<typename Predicate>
    inline SiblingIterator findElement(SiblingIterator first, Predicate predicate) {
        while (!first.done() && !predicate(*first))
            ++first;
        return first;
    }

    /** A predicate for findElement that matches on the field name of
     *  Elements.
     */
    class FieldNameEquals {
    public:
        /** The lifetime of this object must be a subset of the
         *  lifetime of 'fieldName'.
         */
        explicit FieldNameEquals(const StringData& fieldName)
            : _fieldName(fieldName) {}

        bool operator()(const Element& element) const {
            return (_fieldName == element.getFieldName());
        }

    private:
        const StringData& _fieldName;
    };

    /**
     * A convenience wrapper around findElement<FieldNameEquals>.
     */
    inline SiblingIterator findElementNamed(SiblingIterator first, const StringData& fieldName) {
        return findElement(first, FieldNameEquals(fieldName));
    }

    /**
     * Finds the first child under 'parent' that matches the given
     * field name, or returns a done Iterator.
     */
    inline SiblingIterator findFirstChildNamed(Element parent, const StringData& fieldName) {
        return findElementNamed(parent.children(), fieldName);
    }

} // namespace mutablebson
} // namespace mongo
