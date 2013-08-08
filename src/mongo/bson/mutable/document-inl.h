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

namespace mongo {
namespace mutablebson {

    inline int Document::compareWith(const Document& other, bool considerFieldName) const {
        // We cheat and use Element::compareWithElement since we know that 'other' is a
        // Document and has a 'hidden' fieldname that is always indentical across all Document
        // instances.
        return root().compareWithElement(other.root(), considerFieldName);
    }

    inline int Document::compareWithBSONObj(const BSONObj& other, bool considerFieldName) const {
        return root().compareWithBSONObj(other, considerFieldName);
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

} // namespace mutablebson
} // namespace mongo
