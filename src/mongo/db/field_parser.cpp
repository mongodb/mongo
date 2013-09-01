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

    template<class T>
    void _genFieldErrMsg(const BSONObj& doc,
                         const BSONField<T>& field,
                         const string expected,
                         string* errMsg)
    {
        if (!errMsg) return;
        *errMsg = stream() << "wrong type for '" << field() << "' field, expected " << expected
                           << ", found " << doc[field.name()].toString();
    }

    FieldParser::FieldState FieldParser::extract(BSONObj doc,
                              const BSONField<bool>& field,
                              bool* out,
                              string* errMsg)
    {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            if (field.hasDefault()) {
                *out = field.getDefault();
                return FIELD_DEFAULT;
            }
            else {
                return FIELD_NONE;
            }
        }

        if (elem.type() == Bool) {
            *out = elem.boolean();
            return FIELD_SET;
        }

        _genFieldErrMsg(doc, field, "boolean", errMsg);
        return FIELD_INVALID;
    }

    FieldParser::FieldState FieldParser::extract(BSONObj doc,
                              const BSONField<BSONArray>& field,
                              BSONArray* out,
                              string* errMsg)
    {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            if (field.hasDefault()) {
                *out = field.getDefault();
                return FIELD_DEFAULT;
            }
            else {
                return FIELD_NONE;
            }
        }

        if (elem.type() == Array) {
            *out = BSONArray(elem.embeddedObject().getOwned());
            return FIELD_SET;
        }

        _genFieldErrMsg(doc, field, "array", errMsg);
        return FIELD_INVALID;
    }

    FieldParser::FieldState FieldParser::extract(BSONObj doc,
                              const BSONField<BSONObj>& field,
                              BSONObj* out,
                              string* errMsg)
    {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            if (field.hasDefault()) {
                *out = field.getDefault();
                return FIELD_DEFAULT;
            }
            else {
                return FIELD_NONE;
            }
        }

        if (elem.type() == Object) {
            *out = elem.embeddedObject().getOwned();
            return FIELD_SET;
        }

        _genFieldErrMsg(doc, field, "object", errMsg);
        return FIELD_INVALID;
    }

    FieldParser::FieldState FieldParser::extract(BSONObj doc,
                              const BSONField<Date_t>& field,
                              Date_t* out,
                              string* errMsg)
    {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            if (field.hasDefault()) {
                *out = field.getDefault();
                return FIELD_DEFAULT;
            }
            else {
                return FIELD_NONE;
            }
        }

        if (elem.type() == Date) {
            *out = elem.date();
            return FIELD_SET;
        }

        _genFieldErrMsg(doc, field, "date or timestamp", errMsg);
        return FIELD_INVALID;
    }

    FieldParser::FieldState FieldParser::extract(BSONObj doc,
                              const BSONField<string>& field,
                              string* out,
                              string* errMsg)
    {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            if (field.hasDefault()) {
                *out = field.getDefault();
                return FIELD_DEFAULT;
            }
            else {
                return FIELD_NONE;
            }
        }

        if (elem.type() == String) {
            *out = elem.valuestr();
            return FIELD_SET;
        }

        _genFieldErrMsg(doc, field, "string", errMsg);
        return FIELD_INVALID;
    }

    FieldParser::FieldState FieldParser::extract(BSONObj doc,
                              const BSONField<OID>& field,
                              OID* out,
                              string* errMsg)
    {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            if (field.hasDefault()) {
                *out = field.getDefault();
                return FIELD_DEFAULT;
            }
            else {
                return FIELD_NONE;
            }
        }

        if (elem.type() == jstOID) {
            *out = elem.__oid();
            return FIELD_SET;
        }

        _genFieldErrMsg(doc, field, "OID", errMsg);
        return FIELD_INVALID;
    }

    FieldParser::FieldState FieldParser::extract(BSONObj doc,
                              const BSONField<int>& field,
                              int* out,
                              string* errMsg)
    {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            if (field.hasDefault()) {
                *out = field.getDefault();
                return FIELD_DEFAULT;
            }
            else {
                return FIELD_NONE;
            }
        }

        if (elem.type() == NumberInt) {
            *out = elem.numberInt();
            return FIELD_SET;
        }

        _genFieldErrMsg(doc, field, "integer", errMsg);
        return FIELD_INVALID;
    }

    FieldParser::FieldState FieldParser::extractNumber(BSONObj doc,
                              const BSONField<int>& field,
                              int* out,
                              string* errMsg)
    {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            if (field.hasDefault()) {
                *out = field.getDefault();
                return FIELD_DEFAULT;
            }
            else {
                return FIELD_NONE;
            }
        }

        if (elem.isNumber()) {
            *out = elem.numberInt();
            return FIELD_SET;
        }

        _genFieldErrMsg(doc, field, "number", errMsg);
        return FIELD_INVALID;
    }

    FieldParser::FieldState FieldParser::extract(BSONObj doc,
                              const BSONField<long long>& field,
                              long long* out,
                              string* errMsg)
    {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            if (field.hasDefault()) {
                *out = field.getDefault();
                return FIELD_DEFAULT;
            }
            else {
                return FIELD_NONE;
            }
        }

        if (elem.type() == NumberLong) {
            *out = elem.numberLong();
            return FIELD_SET;
        }

        _genFieldErrMsg(doc, field, "long", errMsg);
        return FIELD_INVALID;
    }

    FieldParser::FieldState FieldParser::extractNumber(BSONObj doc,
                                    const BSONField<long long>& field,
                                    long long* out,
                                    string* errMsg)
    {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            if (field.hasDefault()) {
                *out = field.getDefault();
                return FIELD_DEFAULT;
            }
            else {
                return FIELD_NONE;
            }
        }

        if (elem.isNumber()) {
            *out = elem.numberLong();
            return FIELD_SET;
        }

        _genFieldErrMsg(doc, field, "number", errMsg);
        return FIELD_INVALID;
    }

} // namespace mongo
