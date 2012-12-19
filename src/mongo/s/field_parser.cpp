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
 */

#include "mongo/s/field_parser.h"
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
                              bool def,
                              bool* out,
                              string* errMsg)
    {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            *out = def;
            return FIELD_NONE;
        }

        if (elem.type() == Bool) {
            *out = elem.boolean();
            return FIELD_VALID;
        }

        _genFieldErrMsg(doc, field, "boolean", errMsg);
        return FIELD_INVALID;
    }

    FieldParser::FieldState FieldParser::extract(BSONObj doc,
                              const BSONField<BSONArray>& field,
                              const BSONArray& def,
                              BSONArray* out,
                              string* errMsg)
    {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            *out = def;
            return FIELD_NONE;
        }

        if (elem.type() == Array) {
            *out = BSONArray(elem.embeddedObject().getOwned());
            return FIELD_VALID;
        }

        _genFieldErrMsg(doc, field, "array", errMsg);
        return FIELD_INVALID;
    }

    FieldParser::FieldState FieldParser::extract(BSONObj doc,
                              const BSONField<BSONObj>& field,
                              const BSONObj& def,
                              BSONObj* out,
                              string* errMsg)
    {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            *out = def;
            return FIELD_NONE;
        }

        if (elem.type() == Object) {
            *out = elem.embeddedObject().getOwned();
            return FIELD_VALID;
        }

        _genFieldErrMsg(doc, field, "object", errMsg);
        return FIELD_INVALID;
    }

    FieldParser::FieldState FieldParser::extract(BSONObj doc,
                              const BSONField<Date_t>& field,
                              const Date_t def,
                              Date_t* out,
                              string* errMsg)
    {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            *out = def;
            return FIELD_NONE;
        }

        if (elem.type() == Date) {
            *out = elem.date();
            return FIELD_VALID;
        }

        _genFieldErrMsg(doc, field, "date or timestamp", errMsg);
        return FIELD_INVALID;
    }

    FieldParser::FieldState FieldParser::extract(BSONObj doc,
                              const BSONField<string>& field,
                              const string& def,
                              string* out,
                              string* errMsg)
    {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            *out = def;
            return FIELD_NONE;
        }

        if (elem.type() == String) {
            *out = elem.valuestr();
            return FIELD_VALID;
        }

        _genFieldErrMsg(doc, field, "string", errMsg);
        return FIELD_INVALID;
    }

    FieldParser::FieldState FieldParser::extract(BSONObj doc,
                              const BSONField<OID>& field,
                              const OID& def,
                              OID* out,
                              string* errMsg)
    {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            *out = def;
            return FIELD_NONE;
        }

        if (elem.type() == jstOID) {
            *out = elem.__oid();
            return FIELD_VALID;
        }

        _genFieldErrMsg(doc, field, "OID", errMsg);
        return FIELD_INVALID;
    }

    FieldParser::FieldState FieldParser::extract(BSONObj doc,
                              const BSONField<int>& field,
                              const int& def,
                              int* out,
                              string* errMsg)
    {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            *out = def;
            return FIELD_NONE;
        }

        if (elem.type() == NumberInt) {
            *out = elem.numberInt();
            return FIELD_VALID;
        }

        _genFieldErrMsg(doc, field, "integer", errMsg);
        return FIELD_INVALID;
    }

    FieldParser::FieldState FieldParser::extractNumber(BSONObj doc,
                                    const BSONField<int>& field,
                                    const int& def,
                                    int* out,
                                    string* errMsg)
    {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            *out = def;
            return FIELD_NONE;
        }

        if (elem.isNumber()) {
            *out = elem.numberInt();
            return FIELD_VALID;
        }

        _genFieldErrMsg(doc, field, "number", errMsg);
        return FIELD_INVALID;
    }

    FieldParser::FieldState FieldParser::extract(BSONObj doc,
                              const BSONField<long long>& field,
                              const long long& def,
                              long long* out,
                              string* errMsg)
    {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            *out = def;
            return FIELD_NONE;
        }

        if (elem.type() == NumberLong) {
            *out = elem.numberLong();
            return FIELD_VALID;
        }

        _genFieldErrMsg(doc, field, "long", errMsg);
        return FIELD_INVALID;
    }

    FieldParser::FieldState FieldParser::extractNumber(BSONObj doc,
                                    const BSONField<long long>& field,
                                    const long long& def,
                                    long long* out,
                                    string* errMsg)
    {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            *out = def;
            return FIELD_NONE;
        }

        if (elem.isNumber()) {
            *out = elem.numberLong();
            return FIELD_VALID;
        }

        _genFieldErrMsg(doc, field, "number", errMsg);
        return FIELD_INVALID;
    }

} // namespace mongo
