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

#include <iosfwd>

#include "mongo/bson/mutable/document.h"

namespace mongo {

    class BSONObj;

namespace mutablebson {

    //
    // Utilities for mutable BSON unit tests.
    //

    /**
     * Catch all comparator between a mutable 'doc' and the expected BSON 'exp'. It compares
     * (a) 'doc's generated object, (b) 'exp', the expected object, and (c) 'doc(exp)', a
     * document created from 'exp'. Returns true if all three are equal, otherwise false.
     */
    bool checkDoc(const Document& lhs, const BSONObj& rhs);
    bool checkDoc(const Document& lhs, const Document& rhs);

    inline bool operator==(const Document& lhs, const Document& rhs) {
        return checkDoc(lhs, rhs);
    }

    inline bool operator==(const BSONObj& lhs, const Document& rhs) {
        return checkDoc(rhs, lhs);
    }

    inline bool operator==(const Document& lhs, const BSONObj& rhs) {
        return checkDoc(lhs, rhs);
    }

    /** Stream out a document; useful within ASSERT calls */
    std::ostream& operator<<(std::ostream& stream, const Document& doc);

    /** Stream out an element; useful within ASSERT calls */
    std::ostream& operator<<(std::ostream& stream, const ConstElement& elt);

    /** Check that the two provided Documents are equivalent modulo field ordering in Object
     *  Elements. Leaf values are considered equal via woCompare.
     */
    bool checkEqualNoOrdering(const Document& lhs, const Document& rhs);

    struct UnorderedWrapper_Obj {
        inline explicit UnorderedWrapper_Obj(const BSONObj& o)
            : obj(o) {}
        const BSONObj& obj;
    };

    struct UnorderedWrapper_Doc {
        inline explicit UnorderedWrapper_Doc(const Document& d)
            : doc(d) {}
        const Document& doc;
    };

    inline UnorderedWrapper_Doc unordered(const Document& d) {
        return UnorderedWrapper_Doc(d);
    }

    inline UnorderedWrapper_Obj unordered(const BSONObj& o) {
        return UnorderedWrapper_Obj(o);
    }

    inline bool operator==(const UnorderedWrapper_Doc& lhs, const UnorderedWrapper_Doc& rhs) {
        return checkEqualNoOrdering(lhs.doc, rhs.doc);
    }

    inline bool operator!=(const UnorderedWrapper_Doc& lhs, const UnorderedWrapper_Doc& rhs) {
        return !(lhs == rhs);
    }


    inline bool operator==(const UnorderedWrapper_Obj& lhs, const UnorderedWrapper_Obj& rhs) {
        const Document dlhs(lhs.obj);
        const Document drhs(rhs.obj);
        return checkEqualNoOrdering(dlhs, drhs);
    }

    inline bool operator!=(const UnorderedWrapper_Obj& lhs, const UnorderedWrapper_Obj& rhs) {
        return !(lhs == rhs);
    }


    inline bool operator==(const UnorderedWrapper_Doc& lhs, const UnorderedWrapper_Obj& rhs) {
        const Document drhs(rhs.obj);
        return checkEqualNoOrdering(lhs.doc, drhs);
    }

    inline bool operator!=(const UnorderedWrapper_Doc& lhs, const UnorderedWrapper_Obj& rhs) {
        return !(lhs == rhs);
    }


    inline bool operator==(const UnorderedWrapper_Obj& lhs, const UnorderedWrapper_Doc& rhs) {
        const Document dlhs(lhs.obj);
        return checkEqualNoOrdering(dlhs, rhs.doc);
    }

    inline bool operator!=(const UnorderedWrapper_Obj& lhs, const UnorderedWrapper_Doc& rhs) {
        return !(lhs == rhs);
    }

    std::ostream& operator<<(std::ostream& stream, const UnorderedWrapper_Doc& uw_d);
    std::ostream& operator<<(std::ostream& stream, const UnorderedWrapper_Obj& uw_o);

} // namespace mutablebson
} // namespace mongo
