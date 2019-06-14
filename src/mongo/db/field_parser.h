/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#pragma once

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

namespace mongo {

class FieldParser {
private:
    template <typename T>
    static void _genFieldErrMsg(const BSONElement& elem,
                                const BSONField<T>& field,
                                StringData expected,
                                std::string* errMsg) {
        using namespace fmt::literals;
        if (!errMsg)
            return;
        *errMsg = "wrong type for '{}' field, expected {}, found {}"_format(
            field(), expected, elem.toString());
    }

public:
    /**
     * Returns true and fills in 'out' with the contents of the field described by 'field'
     * or with the value in 'def', depending on whether the field is present and has the
     * correct type in 'doc' or not, respectively. Otherwise, if the field exists but has
     * the wrong type, returns false.
     *
     * NOTE ON BSON OWNERSHIP:
     *
     *   The caller must assume that this class will point to data inside 'doc' without
     *   copying it. In practice this means that 'doc' MUST EXIST for as long as 'out'
     *   stays in scope.
     */

    enum FieldState {
        // The field is present but has the wrong type
        FIELD_INVALID = 0,

        // The field is present and has the correct type
        FIELD_SET,

        // The field is absent in the BSON object but set from default
        FIELD_DEFAULT,

        // The field is absent and no default was specified
        FIELD_NONE
    };

    static FieldState extract(BSONObj doc,
                              const BSONField<bool>& field,
                              bool* out,
                              std::string* errMsg = nullptr);

    static FieldState extract(BSONElement elem,
                              const BSONField<bool>& field,
                              bool* out,
                              std::string* errMsg = nullptr);

    static FieldState extract(BSONObj doc,
                              const BSONField<BSONArray>& field,
                              BSONArray* out,
                              std::string* errMsg = nullptr);

    static FieldState extract(BSONElement elem,
                              const BSONField<BSONArray>& field,
                              BSONArray* out,
                              std::string* errMsg = nullptr);

    static FieldState extract(BSONObj doc,
                              const BSONField<BSONObj>& field,
                              BSONObj* out,
                              std::string* errMsg = nullptr);

    static FieldState extract(BSONElement elem,
                              const BSONField<BSONObj>& field,
                              BSONObj* out,
                              std::string* errMsg = nullptr);

    static FieldState extract(BSONObj doc,
                              const BSONField<Date_t>& field,
                              Date_t* out,
                              std::string* errMsg = nullptr);

    static FieldState extract(BSONElement elem,
                              const BSONField<Date_t>& field,
                              Date_t* out,
                              std::string* errMsg = nullptr);

    static FieldState extract(BSONObj doc,
                              const BSONField<Timestamp>& field,
                              Timestamp* out,
                              std::string* errMsg = nullptr);

    static FieldState extract(BSONElement elem,
                              const BSONField<Timestamp>& field,
                              Timestamp* out,
                              std::string* errMsg = nullptr);

    static FieldState extract(BSONObj doc,
                              const BSONField<std::string>& field,
                              std::string* out,
                              std::string* errMsg = nullptr);

    static FieldState extract(BSONElement elem,
                              const BSONField<std::string>& field,
                              std::string* out,
                              std::string* errMsg = nullptr);

    static FieldState extract(BSONObj doc,
                              const BSONField<OID>& field,
                              OID* out,
                              std::string* errMsg = nullptr);

    static FieldState extract(BSONElement elem,
                              const BSONField<OID>& field,
                              OID* out,
                              std::string* errMsg = nullptr);

    static FieldState extract(BSONObj doc,
                              const BSONField<int>& field,
                              int* out,
                              std::string* errMsg = nullptr);

    static FieldState extract(BSONElement elem,
                              const BSONField<int>& field,
                              int* out,
                              std::string* errMsg = nullptr);

    static FieldState extract(BSONObj doc,
                              const BSONField<long long>& field,
                              long long* out,
                              std::string* errMsg = nullptr);

    static FieldState extract(BSONElement elem,
                              const BSONField<long long>& field,
                              long long* out,
                              std::string* errMsg = nullptr);

    static FieldState extract(BSONElement elem,
                              const BSONField<double>& field,
                              double* out,
                              std::string* errMsg = nullptr);

    static FieldState extract(BSONObj doc,
                              const BSONField<double>& field,
                              double* out,
                              std::string* errMsg = nullptr);

    /**
     * The following extractNumber methods do implicit conversion between any numeric type and
     * the BSONField type.  This can be useful when an exact numeric type is not needed, for
     * example if the field is sometimes modified from the shell which can change the type.
     */
    static FieldState extractNumber(BSONObj doc,
                                    const BSONField<int>& field,
                                    int* out,
                                    std::string* errMsg = nullptr);

    static FieldState extractNumber(BSONElement elem,
                                    const BSONField<int>& field,
                                    int* out,
                                    std::string* errMsg = nullptr);

    static FieldState extractNumber(BSONObj doc,
                                    const BSONField<long long>& field,
                                    long long* out,
                                    std::string* errMsg = nullptr);

    static FieldState extractNumber(BSONElement elem,
                                    const BSONField<long long>& field,
                                    long long* out,
                                    std::string* errMsg = nullptr);

    static FieldState extractNumber(BSONObj doc,
                                    const BSONField<double>& field,
                                    double* out,
                                    std::string* errMsg = nullptr);

    static FieldState extractNumber(BSONElement elem,
                                    const BSONField<double>& field,
                                    double* out,
                                    std::string* errMsg = nullptr);

    /**
     * Extracts a document id from a particular field name, which may be of any type but Array.
     * Wraps the extracted id value in a BSONObj with one element and empty field name.
     */
    static FieldState extractID(BSONObj doc,
                                const BSONField<BSONObj>& field,
                                BSONObj* out,
                                std::string* errMsg = nullptr);

    static FieldState extractID(BSONElement elem,
                                const BSONField<BSONObj>& field,
                                BSONObj* out,
                                std::string* errMsg = nullptr);

    // TODO: BSONElement extraction of types below

    /**
     * Extracts a mandatory 'field' from the object 'doc'. Writes the extracted contents to '*out'
     * if successful or fills '*errMsg', if exising, otherwise. This variant relies on T having a
     * parseBSON method.
     */
    template <typename T>
    static FieldState extract(BSONObj doc,
                              const BSONField<T>& field,
                              T* out,
                              std::string* errMsg = nullptr);

    /**
     * Similar to the mandatory 'extract' but on a optional field. The '*out' value would only be
     * allocated if the field is present. The ownership of '*out' would be transferred to the
     * caller, in that case.
     */
    template <typename T>
    static FieldState extract(BSONObj doc,
                              const BSONField<T*>& field,
                              T** out,
                              std::string* errMsg = nullptr);

    template <typename T>
    static FieldState extract(BSONObj doc,
                              const BSONField<T>& field,
                              T** out,  // alloc variation
                              std::string* errMsg = nullptr);

    /**
     * Extracts a mandatory repetition of 'field', from the object 'doc'. Writes the extracted
     * contents to '*out' if successful or fills '*errMsg', if exising, otherwise.  This variant
     * relies on T having a parseBSON method.
     *
     * The vector owns the instances of T.
     */
    template <typename T>
    static FieldState extract(BSONObj doc,
                              const BSONField<std::vector<T*>>& field,
                              std::vector<T*>* out,
                              std::string* errMsg = nullptr);

    /**
     * Extracts a mandatory repetition of 'field', from the field 'elem'. Writes the extracted
     * contents to '*out' if successful or fills '*errMsg', if exising, otherwise.  This variant
     * relies on T having a parseBSON method.
     *
     * The vector owns the instances of T.
     */
    template <typename T>
    static FieldState extract(BSONElement elem,
                              const BSONField<std::vector<T*>>& field,
                              std::vector<T*>* out,
                              std::string* errMsg = nullptr);

    /**
     * Similar to the mandatory repetition' extract but on an optional field. The '*out' value would
     * only be allocated if the field is present. The ownership of '*out' would be transferred to
     * the caller, in that case.
     *
     * The vector owns the instances of T.
     */
    template <typename T>
    static FieldState extract(BSONObj doc,
                              const BSONField<std::vector<T*>>& field,
                              std::vector<T*>** out,
                              std::string* errMsg = nullptr);

    //
    // ==================== Below DEPRECATED; use types instead ====================
    //

    /**
     * The following extract methods are templatized to handle extraction of vectors and
     * maps of sub-objects.  Keys in the map should be StringData compatible.
     *
     * It's possible to nest extraction of vectors and maps to any depth, i.e:
     *
     * std::vector<map<std::string,vector<std::string> > > val;
     * FieldParser::extract(doc, field, val, &val);
     */
    template <typename T>
    static FieldState extract(BSONObj doc,
                              const BSONField<std::vector<T>>& field,
                              std::vector<T>* out,
                              std::string* errMsg = nullptr);

    template <typename T>
    static FieldState extract(BSONElement elem,
                              const BSONField<std::vector<T>>& field,
                              std::vector<T>* out,
                              std::string* errMsg = nullptr);

    template <typename K, typename T>
    static FieldState extract(BSONObj doc,
                              const BSONField<std::map<K, T>>& field,
                              std::map<K, T>* out,
                              std::string* errMsg = nullptr);

    template <typename K, typename T>
    static FieldState extract(BSONElement elem,
                              const BSONField<std::map<K, T>>& field,
                              std::map<K, T>* out,
                              std::string* errMsg = nullptr);
};

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
        _genFieldErrMsg(elem, field(), "vector or array", errMsg);
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
    using namespace fmt::literals;
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
                    *errMsg = "error parsing element {} of field {}{}"_format(
                        i, field(), causedBy(elErrMsg));
                }
                return FIELD_INVALID;
            }
            i++;
        }

        return FIELD_SET;
    }

    _genFieldErrMsg(elem, field, "vector array", errMsg);
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
        _genFieldErrMsg(elem, field, "vector array", errMsg);
        return FIELD_INVALID;
    }

    BSONArray arr = BSONArray(elem.embeddedObject());
    BSONObjIterator objIt(arr);
    while (objIt.more()) {
        BSONElement next = objIt.next();

        if (next.type() != Object) {
            _genFieldErrMsg(elem, field, "object", errMsg);
            return FIELD_INVALID;
        }

        std::unique_ptr<T> toInsert(new T);

        if (!toInsert->parseBSON(next.embeddedObject(), errMsg)) {
            return FIELD_INVALID;
        }

        out->push_back(toInsert.release());
    }

    return FIELD_SET;
}

template <typename T>
FieldParser::FieldState FieldParser::extract(BSONObj doc,
                                             const BSONField<std::vector<T*>>& field,
                                             std::vector<T*>** out,
                                             std::string* errMsg) {
    using namespace fmt::literals;
    dassert(!field.hasDefault());

    BSONElement elem = doc[field.name()];
    if (elem.eoo()) {
        return FIELD_NONE;
    }

    if (elem.type() != Array) {
        _genFieldErrMsg(elem, field, "vector array", errMsg);
        return FIELD_INVALID;
    }

    auto tempVector = std::make_unique<std::vector<T*>>();
    auto guard = makeGuard([&tempVector] {
        if (tempVector) {
            for (T*& raw : *tempVector) {
                delete raw;
            }
        }
    });

    BSONArray arr = BSONArray(elem.embeddedObject());
    BSONObjIterator objIt(arr);
    while (objIt.more()) {
        BSONElement next = objIt.next();

        if (next.type() != Object) {
            if (errMsg) {
                *errMsg = "wrong type for '{}' field contents, expected object, found {}"_format(
                    field(), elem.type());
            }
            return FIELD_INVALID;
        }

        std::unique_ptr<T> toInsert(new T);
        if (!toInsert->parseBSON(next.embeddedObject(), errMsg)) {
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
    using namespace fmt::literals;
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
                    *errMsg = "error parsing map element {} of field {}{}"_format(
                        next.fieldName(), field(), causedBy(elErrMsg));
                }
                return FIELD_INVALID;
            }
        }

        return FIELD_SET;
    }

    _genFieldErrMsg(elem, field, "vector array", errMsg);
    return FIELD_INVALID;
}

}  // namespace mongo
