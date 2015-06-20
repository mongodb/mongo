/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using mongoutils::str::stream;

template <class T>
void _genFieldErrMsg(const BSONElement& elem,
                     const BSONField<T>& field,
                     const std::string expected,
                     std::string* errMsg) {
    if (!errMsg)
        return;
    *errMsg = stream() << "wrong type for '" << field() << "' field, expected " << expected
                       << ", found " << elem.toString();
}

template <typename T>
FieldParser::FieldState FieldParser::extract(BSONObj doc,
                                             const BSONField<T>& field,
                                             T* out,
                                             std::string* errMsg) {
    BSONElement elem = doc[field.name()];
    if (elem.eoo()) {
        if (field.hasDefault()) {
            field.getDefault().cloneTo(out);
            return FIELD_DEFAULT;
        } else {
            return FIELD_NONE;
        }
    }

    if (elem.type() != Object && elem.type() != Array) {
        _genFieldErrMsg(elem, field, "Object/Array", errMsg);
        return FIELD_INVALID;
    }

    if (!out->parseBSON(elem.embeddedObject(), errMsg)) {
        return FIELD_INVALID;
    }

    return FIELD_SET;
}

template <typename T>
FieldParser::FieldState FieldParser::extract(BSONObj doc,
                                             const BSONField<T*>& field,
                                             T** out,
                                             std::string* errMsg) {
    BSONElement elem = doc[field.name()];
    if (elem.eoo()) {
        if (field.hasDefault()) {
            std::unique_ptr<T> temp(new T);
            field.getDefault()->cloneTo(temp.get());

            *out = temp.release();
            return FIELD_DEFAULT;
        } else {
            return FIELD_NONE;
        }
    }

    if (elem.type() != Object && elem.type() != Array) {
        _genFieldErrMsg(elem, field, "Object/Array", errMsg);
        return FIELD_INVALID;
    }

    std::unique_ptr<T> temp(new T);
    if (!temp->parseBSON(elem.embeddedObject(), errMsg)) {
        return FIELD_INVALID;
    }

    *out = temp.release();
    return FIELD_SET;
}

template <typename T>
FieldParser::FieldState FieldParser::extract(BSONObj doc,
                                             const BSONField<T>& field,
                                             T** out,
                                             std::string* errMsg) {
    BSONElement elem = doc[field.name()];
    if (elem.eoo()) {
        if (field.hasDefault()) {
            *out = new T;
            field.getDefault().cloneTo(*out);
            return FIELD_DEFAULT;
        } else {
            return FIELD_NONE;
        }
    }

    if (elem.type() != Object && elem.type() != Array) {
        if (errMsg) {
            *errMsg = stream() << "wrong type for '" << field() << "' field, expected "
                               << "vector or array"
                               << ", found " << doc[field.name()].toString();
        }
        return FIELD_INVALID;
    }

    std::unique_ptr<T> temp(new T);
    if (!temp->parseBSON(elem.embeddedObject(), errMsg)) {
        return FIELD_INVALID;
    }

    *out = temp.release();
    return FIELD_SET;
}

// Extracts an array into a vector
template <typename T>
FieldParser::FieldState FieldParser::extract(BSONObj doc,
                                             const BSONField<std::vector<T>>& field,
                                             std::vector<T>* out,
                                             std::string* errMsg) {
    return extract(doc[field.name()], field, out, errMsg);
}

template <typename T>
FieldParser::FieldState FieldParser::extract(BSONElement elem,
                                             const BSONField<std::vector<T>>& field,
                                             std::vector<T>* out,
                                             std::string* errMsg) {
    if (elem.eoo()) {
        if (field.hasDefault()) {
            *out = field.getDefault();
            return FIELD_DEFAULT;
        } else {
            return FIELD_NONE;
        }
    }

    if (elem.type() == Array) {
        BSONArray arr = BSONArray(elem.embeddedObject());
        std::string elErrMsg;

        // Append all the new elements to the end of the vector
        size_t initialSize = out->size();
        out->resize(initialSize + arr.nFields());

        int i = 0;
        BSONObjIterator objIt(arr);
        while (objIt.more()) {
            BSONElement next = objIt.next();
            BSONField<T> fieldFor(next.fieldName(), out->at(initialSize + i));

            if (!FieldParser::extract(next, fieldFor, &out->at(initialSize + i), &elErrMsg)) {
                if (errMsg) {
                    *errMsg = stream() << "error parsing element " << i << " of field " << field()
                                       << causedBy(elErrMsg);
                }
                return FIELD_INVALID;
            }
            i++;
        }

        return FIELD_SET;
    }

    if (errMsg) {
        *errMsg = stream() << "wrong type for '" << field() << "' field, expected "
                           << "vector array"
                           << ", found " << elem.toString();
    }
    return FIELD_INVALID;
}

template <typename T>
FieldParser::FieldState FieldParser::extract(BSONObj doc,
                                             const BSONField<std::vector<T*>>& field,
                                             std::vector<T*>* out,
                                             std::string* errMsg) {
    dassert(!field.hasDefault());

    BSONElement elem = doc[field.name()];
    if (elem.eoo()) {
        return FIELD_NONE;
    }

    return extract(elem, field, out, errMsg);
}

template <typename T>
FieldParser::FieldState FieldParser::extract(BSONElement elem,
                                             const BSONField<std::vector<T*>>& field,
                                             std::vector<T*>* out,
                                             std::string* errMsg) {
    if (elem.type() != Array) {
        if (errMsg) {
            *errMsg = stream() << "wrong type for '" << field() << "' field, expected "
                               << "vector array"
                               << ", found " << elem.toString();
        }
        return FIELD_INVALID;
    }

    BSONArray arr = BSONArray(elem.embeddedObject());
    BSONObjIterator objIt(arr);
    while (objIt.more()) {
        BSONElement next = objIt.next();

        if (next.type() != Object) {
            if (errMsg) {
                *errMsg = stream() << "wrong type for '" << field() << "' field contents, "
                                   << "expected object, found " << elem.type();
            }
            return FIELD_INVALID;
        }

        std::unique_ptr<T> toInsert(new T);

        if (!toInsert->parseBSON(next.embeddedObject(), errMsg) || !toInsert->isValid(errMsg)) {
            return FIELD_INVALID;
        }

        out->push_back(toInsert.release());
    }

    return FIELD_SET;
}

template <typename T>
void FieldParser::clearOwnedVector(std::vector<T*>* vec) {
    for (typename std::vector<T*>::iterator it = vec->begin(); it != vec->end(); ++it) {
        delete (*it);
    }
}

template <typename T>
FieldParser::FieldState FieldParser::extract(BSONObj doc,
                                             const BSONField<std::vector<T*>>& field,
                                             std::vector<T*>** out,
                                             std::string* errMsg) {
    dassert(!field.hasDefault());

    BSONElement elem = doc[field.name()];
    if (elem.eoo()) {
        return FIELD_NONE;
    }

    if (elem.type() != Array) {
        if (errMsg) {
            *errMsg = stream() << "wrong type for '" << field() << "' field, expected "
                               << "vector array"
                               << ", found " << doc[field.name()].toString();
        }
        return FIELD_INVALID;
    }

    std::unique_ptr<std::vector<T*>> tempVector(new std::vector<T*>);

    BSONArray arr = BSONArray(elem.embeddedObject());
    BSONObjIterator objIt(arr);
    while (objIt.more()) {
        BSONElement next = objIt.next();

        if (next.type() != Object) {
            if (errMsg) {
                *errMsg = stream() << "wrong type for '" << field() << "' field contents, "
                                   << "expected object, found " << elem.type();
            }
            clearOwnedVector(tempVector.get());
            return FIELD_INVALID;
        }

        std::unique_ptr<T> toInsert(new T);
        if (!toInsert->parseBSON(next.embeddedObject(), errMsg)) {
            clearOwnedVector(tempVector.get());
            return FIELD_INVALID;
        }

        tempVector->push_back(toInsert.release());
    }

    *out = tempVector.release();
    return FIELD_SET;
}

// Extracts an object into a map
template <typename K, typename T>
FieldParser::FieldState FieldParser::extract(BSONObj doc,
                                             const BSONField<std::map<K, T>>& field,
                                             std::map<K, T>* out,
                                             std::string* errMsg) {
    return extract(doc[field.name()], field, out, errMsg);
}

template <typename K, typename T>
FieldParser::FieldState FieldParser::extract(BSONElement elem,
                                             const BSONField<std::map<K, T>>& field,
                                             std::map<K, T>* out,
                                             std::string* errMsg) {
    if (elem.eoo()) {
        if (field.hasDefault()) {
            *out = field.getDefault();
            return FIELD_DEFAULT;
        } else {
            return FIELD_NONE;
        }
    }

    if (elem.type() == Object) {
        BSONObj obj = elem.embeddedObject();
        std::string elErrMsg;

        BSONObjIterator objIt(obj);
        while (objIt.more()) {
            BSONElement next = objIt.next();
            T& value = (*out)[next.fieldName()];

            BSONField<T> fieldFor(next.fieldName(), value);
            if (!FieldParser::extract(next, fieldFor, &value, &elErrMsg)) {
                if (errMsg) {
                    *errMsg = stream() << "error parsing map element " << next.fieldName()
                                       << " of field " << field() << causedBy(elErrMsg);
                }
                return FIELD_INVALID;
            }
        }

        return FIELD_SET;
    }

    if (errMsg) {
        *errMsg = stream() << "wrong type for '" << field() << "' field, expected "
                           << "vector array"
                           << ", found " << elem.toString();
    }
    return FIELD_INVALID;
}

}  // namespace mongo
