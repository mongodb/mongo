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

#pragma once

#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/s/bson_serializable.h"
#include "mongo/util/time_support.h"

namespace mongo {

class FieldParser {
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
                              std::string* errMsg = NULL);

    static FieldState extract(BSONElement elem,
                              const BSONField<bool>& field,
                              bool* out,
                              std::string* errMsg = NULL);

    static FieldState extract(BSONObj doc,
                              const BSONField<BSONArray>& field,
                              BSONArray* out,
                              std::string* errMsg = NULL);

    static FieldState extract(BSONElement elem,
                              const BSONField<BSONArray>& field,
                              BSONArray* out,
                              std::string* errMsg = NULL);

    static FieldState extract(BSONObj doc,
                              const BSONField<BSONObj>& field,
                              BSONObj* out,
                              std::string* errMsg = NULL);

    static FieldState extract(BSONElement elem,
                              const BSONField<BSONObj>& field,
                              BSONObj* out,
                              std::string* errMsg = NULL);

    static FieldState extract(BSONObj doc,
                              const BSONField<Date_t>& field,
                              Date_t* out,
                              std::string* errMsg = NULL);

    static FieldState extract(BSONElement elem,
                              const BSONField<Date_t>& field,
                              Date_t* out,
                              std::string* errMsg = NULL);

    static FieldState extract(BSONObj doc,
                              const BSONField<Timestamp>& field,
                              Timestamp* out,
                              std::string* errMsg = NULL);

    static FieldState extract(BSONElement elem,
                              const BSONField<Timestamp>& field,
                              Timestamp* out,
                              std::string* errMsg = NULL);

    static FieldState extract(BSONObj doc,
                              const BSONField<std::string>& field,
                              std::string* out,
                              std::string* errMsg = NULL);

    static FieldState extract(BSONElement elem,
                              const BSONField<std::string>& field,
                              std::string* out,
                              std::string* errMsg = NULL);

    static FieldState extract(BSONObj doc,
                              const BSONField<OID>& field,
                              OID* out,
                              std::string* errMsg = NULL);

    static FieldState extract(BSONElement elem,
                              const BSONField<OID>& field,
                              OID* out,
                              std::string* errMsg = NULL);

    static FieldState extract(BSONObj doc,
                              const BSONField<int>& field,
                              int* out,
                              std::string* errMsg = NULL);

    static FieldState extract(BSONElement elem,
                              const BSONField<int>& field,
                              int* out,
                              std::string* errMsg = NULL);

    static FieldState extract(BSONObj doc,
                              const BSONField<long long>& field,
                              long long* out,
                              std::string* errMsg = NULL);

    static FieldState extract(BSONElement elem,
                              const BSONField<long long>& field,
                              long long* out,
                              std::string* errMsg = NULL);

    static FieldState extract(BSONElement elem,
                              const BSONField<double>& field,
                              double* out,
                              std::string* errMsg = NULL);

    static FieldState extract(BSONObj doc,
                              const BSONField<double>& field,
                              double* out,
                              std::string* errMsg = NULL);

    /**
     * The following extractNumber methods do implicit conversion between any numeric type and
     * the BSONField type.  This can be useful when an exact numeric type is not needed, for
     * example if the field is sometimes modified from the shell which can change the type.
     */
    static FieldState extractNumber(BSONObj doc,
                                    const BSONField<int>& field,
                                    int* out,
                                    std::string* errMsg = NULL);

    static FieldState extractNumber(BSONElement elem,
                                    const BSONField<int>& field,
                                    int* out,
                                    std::string* errMsg = NULL);

    static FieldState extractNumber(BSONObj doc,
                                    const BSONField<long long>& field,
                                    long long* out,
                                    std::string* errMsg = NULL);

    static FieldState extractNumber(BSONElement elem,
                                    const BSONField<long long>& field,
                                    long long* out,
                                    std::string* errMsg = NULL);

    static FieldState extractNumber(BSONObj doc,
                                    const BSONField<double>& field,
                                    double* out,
                                    std::string* errMsg = NULL);

    static FieldState extractNumber(BSONElement elem,
                                    const BSONField<double>& field,
                                    double* out,
                                    std::string* errMsg = NULL);

    /**
     * Extracts a document id from a particular field name, which may be of any type but Array.
     * Wraps the extracted id value in a BSONObj with one element and empty field name.
     */
    static FieldState extractID(BSONObj doc,
                                const BSONField<BSONObj>& field,
                                BSONObj* out,
                                std::string* errMsg = NULL);

    static FieldState extractID(BSONElement elem,
                                const BSONField<BSONObj>& field,
                                BSONObj* out,
                                std::string* errMsg = NULL);

    // TODO: BSONElement extraction of types below

    /**
     * Extracts a mandatory BSONSerializable structure 'field' from the object 'doc'. Write
     * the extracted contents to '*out' if successful or fills '*errMsg', if exising,
     * otherwise.  This variant relies on T having a parseBSON, which all
     * BSONSerializable's have.
     *
     * TODO: Tighten for BSONSerializable's only
     */
    template <typename T>
    static FieldState extract(BSONObj doc,
                              const BSONField<T>& field,
                              T* out,
                              std::string* errMsg = NULL);

    template <typename T>
    static FieldState extract(BSONObj doc,
                              const BSONField<T*>& field,
                              T** out,
                              std::string* errMsg = NULL);

    /**
     * Similar to the mandatory 'extract' but on a optional field. '*out' would only be
     * allocated if the field is present. The ownership of '*out' would be transferred to
     * the caller, in that case.
     *
     * TODO: Tighten for BSONSerializable's only
     */
    template <typename T>
    static FieldState extract(BSONObj doc,
                              const BSONField<T>& field,
                              T** out,  // alloc variation
                              std::string* errMsg = NULL);

    /**
     * Extracts a mandatory repetition of BSONSerializable structures, 'field', from the
     * object 'doc'. Write the extracted contents to '*out' if successful or fills
     * '*errMsg', if exising, otherwise.  This variant relies on T having a parseBSON,
     * which all BSONSerializable's have.
     *
     * The vector owns the instances of T.
     *
     * TODO: Tighten for BSONSerializable's only
     */
    template <typename T>
    static FieldState extract(BSONObj doc,
                              const BSONField<std::vector<T*>>& field,
                              std::vector<T*>* out,
                              std::string* errMsg = NULL);

    /**
     * Extracts a mandatory repetition of BSONSerializable structures, 'field', from the
     * field 'elem'. Write the extracted contents to '*out' if successful or fills
     * '*errMsg', if exising, otherwise.  This variant relies on T having a parseBSON,
     * which all BSONSerializable's have.
     *
     * The vector owns the instances of T.
     *
     * TODO: Tighten for BSONSerializable's only
     */
    template <typename T>
    static FieldState extract(BSONElement elem,
                              const BSONField<std::vector<T*>>& field,
                              std::vector<T*>* out,
                              std::string* errMsg = NULL);

    /**
     * Similar to the mandatory repetition' extract but on an optional field. '*out' would
     * only be allocated if the field is present. The ownership of '*out' would be
     * transferred to the caller, in that case.
     *
     * The vector owns the instances of T.
     *
     * TODO: Tighten for BSONSerializable's only
     */
    template <typename T>
    static FieldState extract(BSONObj doc,
                              const BSONField<std::vector<T*>>& field,
                              std::vector<T*>** out,
                              std::string* errMsg = NULL);

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
                              std::string* errMsg = NULL);

    template <typename T>
    static FieldState extract(BSONElement elem,
                              const BSONField<std::vector<T>>& field,
                              std::vector<T>* out,
                              std::string* errMsg = NULL);

    template <typename K, typename T>
    static FieldState extract(BSONObj doc,
                              const BSONField<std::map<K, T>>& field,
                              std::map<K, T>* out,
                              std::string* errMsg = NULL);

    template <typename K, typename T>
    static FieldState extract(BSONElement elem,
                              const BSONField<std::map<K, T>>& field,
                              std::map<K, T>* out,
                              std::string* errMsg = NULL);

private:
    template <typename T>
    static void clearOwnedVector(std::vector<T*>* vec);
};

}  // namespace mongo

// Inline functions for templating
#include "field_parser-inl.h"
