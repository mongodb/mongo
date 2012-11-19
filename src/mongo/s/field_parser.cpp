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

namespace mongo {

    bool FieldParser::extract(BSONObj doc,
                              const BSONField<bool>& field,
                              bool def,
                              bool* out) {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            *out = def;
            return true;
        }

        if (elem.type() == Bool) {
            *out = elem.boolean();
            return true;
        }

        return false;
    }

    bool FieldParser::extract(BSONObj doc,
                              const BSONField<BSONArray>& field,
                              const BSONArray& def,
                              BSONArray* out) {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            *out = def;
            return true;
        }

        if (elem.type() == Array) {
            *out = BSONArray(elem.embeddedObject());
            return true;
        }

        return false;
    }

    bool FieldParser::extract(BSONObj doc,
                              const BSONField<BSONObj>& field,
                              const BSONObj& def,
                              BSONObj* out) {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            *out = def;
            return true;
        }

        if (elem.type() == Object) {
            *out = elem.embeddedObject();
            return true;
        }

        return false;
    }

    bool FieldParser::extract(BSONObj doc,
                              const BSONField<Date_t>& field,
                              const Date_t def,
                              Date_t* out) {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            *out = def;
            return true;
        }

        if (elem.type() == Date) {
            *out = elem.date();
            return true;
        }

        return false;
    }

    bool FieldParser::extract(BSONObj doc,
                              const BSONField<string>& field,
                              const string& def,
                              string* out) {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            *out = def;
            return true;
        }

        if (elem.type() == String) {
            *out = elem.valuestr();
            return true;
        }

        return false;
    }

    bool FieldParser::extract(BSONObj doc,
                              const BSONField<OID>& field,
                              const OID& def,
                              OID* out) {
        BSONElement elem = doc[field.name()];
        if (elem.eoo()) {
            *out = def;
            return true;
        }

        if (elem.type() == jstOID) {
            *out = elem.__oid();
            return true;
        }

        return false;
    }

} // namespace mongo
