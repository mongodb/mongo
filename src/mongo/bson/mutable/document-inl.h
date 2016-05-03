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

#pragma once

namespace mongo {
namespace mutablebson {

inline int Document::compareWith(const Document& other,
                                 bool considerFieldName,
                                 const StringData::ComparatorInterface* comparator) const {
    // We cheat and use Element::compareWithElement since we know that 'other' is a
    // Document and has a 'hidden' fieldname that is always indentical across all Document
    // instances.
    return root().compareWithElement(other.root(), considerFieldName, comparator);
}

inline int Document::compareWithBSONObj(const BSONObj& other,
                                        bool considerFieldName,
                                        const StringData::ComparatorInterface* comparator) const {
    return root().compareWithBSONObj(other, considerFieldName, comparator);
}

inline void Document::writeTo(BSONObjBuilder* builder) const {
    return root().writeTo(builder);
}

inline BSONObj Document::getObject() const {
    BSONObjBuilder builder;
    writeTo(&builder);
    return builder.obj();
}

inline Element Document::root() {
    return _root;
}

inline ConstElement Document::root() const {
    return _root;
}

inline Element Document::end() {
    return Element(this, Element::kInvalidRepIdx);
}

inline ConstElement Document::end() const {
    return const_cast<Document*>(this)->end();
}

inline std::string Document::toString() const {
    return getObject().toString();
}

inline bool Document::isInPlaceModeEnabled() const {
    return getCurrentInPlaceMode() == kInPlaceEnabled;
}

}  // namespace mutablebson
}  // namespace mongo
