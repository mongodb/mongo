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

/** Stream out an constelement; useful within ASSERT calls */
std::ostream& operator<<(std::ostream& stream, const ConstElement& elt);

/** Stream out a document; useful within ASSERT calls */
std::ostream& operator<<(std::ostream& stream, const Document& doc);

/** Stream out an element; useful within ASSERT calls */
std::ostream& operator<<(std::ostream& stream, const Element& elt);

/** Check that the two provided Documents are equivalent modulo field ordering in Object
 *  Elements. Leaf values are considered equal via woCompare.
 */
bool checkEqualNoOrdering(const Document& lhs, const Document& rhs);

struct UnorderedWrapper_Obj {
    inline explicit UnorderedWrapper_Obj(const BSONObj& o) : obj(o) {}
    const BSONObj& obj;
};

struct UnorderedWrapper_Doc {
    inline explicit UnorderedWrapper_Doc(const Document& d) : doc(d) {}
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

}  // namespace mutablebson
}  // namespace mongo
