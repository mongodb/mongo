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

#include "mongo/db/field_parser.h"

#include <algorithm>
#include <limits>

namespace mongo {

using std::string;

FieldParser::FieldState FieldParser::extract(BSONObj doc,
                                             const BSONField<bool>& field,
                                             bool* out,
                                             string* errMsg) {
    return extract(doc[field.name()], field, out, errMsg);
}

FieldParser::FieldState FieldParser::extract(BSONElement elem,
                                             const BSONField<bool>& field,
                                             bool* out,
                                             string* errMsg) {
    if (elem.eoo()) {
        if (field.hasDefault()) {
            *out = field.getDefault();
            return FIELD_DEFAULT;
        } else {
            return FIELD_NONE;
        }
    }

    if (elem.type() == BSONType::boolean) {
        *out = elem.boolean();
        return FIELD_SET;
    }

    _genFieldErrMsg(elem, field, "boolean", errMsg);
    return FIELD_INVALID;
}

FieldParser::FieldState FieldParser::extract(BSONObj doc,
                                             const BSONField<BSONArray>& field,
                                             BSONArray* out,
                                             string* errMsg) {
    return extract(doc[field.name()], field, out, errMsg);
}


FieldParser::FieldState FieldParser::extract(BSONElement elem,
                                             const BSONField<BSONArray>& field,
                                             BSONArray* out,
                                             string* errMsg) {
    if (elem.eoo()) {
        if (field.hasDefault()) {
            *out = field.getDefault();
            return FIELD_DEFAULT;
        } else {
            return FIELD_NONE;
        }
    }

    if (elem.type() == BSONType::array) {
        *out = BSONArray(elem.embeddedObject().getOwned());
        return FIELD_SET;
    }

    _genFieldErrMsg(elem, field, "array", errMsg);
    return FIELD_INVALID;
}

FieldParser::FieldState FieldParser::extract(BSONObj doc,
                                             const BSONField<BSONObj>& field,
                                             BSONObj* out,
                                             string* errMsg) {
    return extract(doc[field.name()], field, out, errMsg);
}


FieldParser::FieldState FieldParser::extract(BSONElement elem,
                                             const BSONField<BSONObj>& field,
                                             BSONObj* out,
                                             string* errMsg) {
    if (elem.eoo()) {
        if (field.hasDefault()) {
            *out = field.getDefault().getOwned();
            return FIELD_DEFAULT;
        } else {
            return FIELD_NONE;
        }
    }

    if (elem.type() == BSONType::object) {
        *out = elem.embeddedObject().getOwned();
        return FIELD_SET;
    }

    _genFieldErrMsg(elem, field, "object", errMsg);
    return FIELD_INVALID;
}

FieldParser::FieldState FieldParser::extract(BSONObj doc,
                                             const BSONField<Date_t>& field,
                                             Date_t* out,
                                             string* errMsg) {
    return extract(doc[field.name()], field, out, errMsg);
}


FieldParser::FieldState FieldParser::extract(BSONElement elem,
                                             const BSONField<Date_t>& field,
                                             Date_t* out,
                                             string* errMsg) {
    if (elem.eoo()) {
        if (field.hasDefault()) {
            *out = field.getDefault();
            return FIELD_DEFAULT;
        } else {
            return FIELD_NONE;
        }
    }

    if (elem.type() == BSONType::date) {
        *out = elem.date();
        return FIELD_SET;
    }

    _genFieldErrMsg(elem, field, "date", errMsg);
    return FIELD_INVALID;
}

FieldParser::FieldState FieldParser::extract(BSONObj doc,
                                             const BSONField<Timestamp>& field,
                                             Timestamp* out,
                                             string* errMsg) {
    return extract(doc[field.name()], field, out, errMsg);
}


FieldParser::FieldState FieldParser::extract(BSONElement elem,
                                             const BSONField<Timestamp>& field,
                                             Timestamp* out,
                                             string* errMsg) {
    if (elem.eoo()) {
        if (field.hasDefault()) {
            *out = field.getDefault();
            return FIELD_DEFAULT;
        } else {
            return FIELD_NONE;
        }
    }

    if (elem.type() == BSONType::timestamp) {
        *out = elem.timestamp();
        return FIELD_SET;
    }

    _genFieldErrMsg(elem, field, "timestamp", errMsg);
    return FIELD_INVALID;
}

FieldParser::FieldState FieldParser::extract(BSONObj doc,
                                             const BSONField<string>& field,
                                             string* out,
                                             string* errMsg) {
    return extract(doc[field.name()], field, out, errMsg);
}

FieldParser::FieldState FieldParser::extract(BSONElement elem,
                                             const BSONField<string>& field,
                                             string* out,
                                             string* errMsg) {
    if (elem.eoo()) {
        if (field.hasDefault()) {
            *out = field.getDefault();
            return FIELD_DEFAULT;
        } else {
            return FIELD_NONE;
        }
    }

    if (elem.type() == BSONType::string) {
        // Extract everything, including embedded null characters.
        *out = elem.str();
        return FIELD_SET;
    }

    _genFieldErrMsg(elem, field, "string", errMsg);
    return FIELD_INVALID;
}

FieldParser::FieldState FieldParser::extract(BSONObj doc,
                                             const BSONField<OID>& field,
                                             OID* out,
                                             string* errMsg) {
    return extract(doc[field.name()], field, out, errMsg);
}


FieldParser::FieldState FieldParser::extract(BSONElement elem,
                                             const BSONField<OID>& field,
                                             OID* out,
                                             string* errMsg) {
    if (elem.eoo()) {
        if (field.hasDefault()) {
            *out = field.getDefault();
            return FIELD_DEFAULT;
        } else {
            return FIELD_NONE;
        }
    }

    if (elem.type() == BSONType::oid) {
        *out = elem.__oid();
        return FIELD_SET;
    }

    _genFieldErrMsg(elem, field, "OID", errMsg);
    return FIELD_INVALID;
}

FieldParser::FieldState FieldParser::extract(BSONObj doc,
                                             const BSONField<int>& field,
                                             int* out,
                                             string* errMsg) {
    return extract(doc[field.name()], field, out, errMsg);
}

FieldParser::FieldState FieldParser::extract(BSONElement elem,
                                             const BSONField<int>& field,
                                             int* out,
                                             string* errMsg) {
    if (elem.eoo()) {
        if (field.hasDefault()) {
            *out = field.getDefault();
            return FIELD_DEFAULT;
        } else {
            return FIELD_NONE;
        }
    }

    if (elem.type() == BSONType::numberInt) {
        *out = elem.numberInt();
        return FIELD_SET;
    }

    _genFieldErrMsg(elem, field, "integer", errMsg);
    return FIELD_INVALID;
}

FieldParser::FieldState FieldParser::extractNumber(BSONObj doc,
                                                   const BSONField<int>& field,
                                                   int* out,
                                                   string* errMsg) {
    return extractNumber(doc[field.name()], field, out, errMsg);
}

FieldParser::FieldState FieldParser::extractNumber(BSONElement elem,
                                                   const BSONField<int>& field,
                                                   int* out,
                                                   string* errMsg) {
    if (elem.eoo()) {
        if (field.hasDefault()) {
            *out = field.getDefault();
            return FIELD_DEFAULT;
        } else {
            return FIELD_NONE;
        }
    }

    if (elem.isNumber()) {
        auto num = std::clamp(elem.safeNumberLong(),
                              static_cast<long long>(std::numeric_limits<int>::min()),
                              static_cast<long long>(std::numeric_limits<int>::max()));
        *out = static_cast<int>(num);
        return FIELD_SET;
    }

    _genFieldErrMsg(elem, field, "number", errMsg);
    return FIELD_INVALID;
}

FieldParser::FieldState FieldParser::extract(BSONObj doc,
                                             const BSONField<long long>& field,
                                             long long* out,
                                             string* errMsg) {
    return extract(doc[field.name()], field, out, errMsg);
}

FieldParser::FieldState FieldParser::extract(BSONElement elem,
                                             const BSONField<long long>& field,
                                             long long* out,
                                             string* errMsg) {
    if (elem.eoo()) {
        if (field.hasDefault()) {
            *out = field.getDefault();
            return FIELD_DEFAULT;
        } else {
            return FIELD_NONE;
        }
    }

    if (elem.type() == BSONType::numberLong) {
        *out = elem.numberLong();
        return FIELD_SET;
    }

    _genFieldErrMsg(elem, field, "long", errMsg);
    return FIELD_INVALID;
}

FieldParser::FieldState FieldParser::extractNumber(BSONObj doc,
                                                   const BSONField<long long>& field,
                                                   long long* out,
                                                   string* errMsg) {
    return extractNumber(doc[field.name()], field, out, errMsg);
}

FieldParser::FieldState FieldParser::extractNumber(BSONElement elem,
                                                   const BSONField<long long>& field,
                                                   long long* out,
                                                   string* errMsg) {
    if (elem.eoo()) {
        if (field.hasDefault()) {
            *out = field.getDefault();
            return FIELD_DEFAULT;
        } else {
            return FIELD_NONE;
        }
    }

    if (elem.isNumber()) {
        *out = elem.safeNumberLong();
        return FIELD_SET;
    }

    _genFieldErrMsg(elem, field, "number", errMsg);
    return FIELD_INVALID;
}

FieldParser::FieldState FieldParser::extract(BSONObj doc,
                                             const BSONField<double>& field,
                                             double* out,
                                             string* errMsg) {
    return extract(doc[field.name()], field, out, errMsg);
}

FieldParser::FieldState FieldParser::extract(BSONElement elem,
                                             const BSONField<double>& field,
                                             double* out,
                                             string* errMsg) {
    if (elem.eoo()) {
        if (field.hasDefault()) {
            *out = field.getDefault();
            return FIELD_DEFAULT;
        } else {
            return FIELD_NONE;
        }
    }

    if (elem.type() == BSONType::numberDouble) {
        *out = elem.numberDouble();
        return FIELD_SET;
    }

    _genFieldErrMsg(elem, field, "double", errMsg);
    return FIELD_INVALID;
}

FieldParser::FieldState FieldParser::extractNumber(BSONObj doc,
                                                   const BSONField<double>& field,
                                                   double* out,
                                                   string* errMsg) {
    return extractNumber(doc[field.name()], field, out, errMsg);
}

FieldParser::FieldState FieldParser::extractNumber(BSONElement elem,
                                                   const BSONField<double>& field,
                                                   double* out,
                                                   string* errMsg) {
    if (elem.eoo()) {
        if (field.hasDefault()) {
            *out = field.getDefault();
            return FIELD_DEFAULT;
        } else {
            return FIELD_NONE;
        }
    }

    if (elem.isNumber()) {
        *out = elem.numberDouble();
        return FIELD_SET;
    }

    _genFieldErrMsg(elem, field, "number", errMsg);
    return FIELD_INVALID;
}

FieldParser::FieldState FieldParser::extractID(BSONObj doc,
                                               const BSONField<BSONObj>& field,
                                               BSONObj* out,
                                               string* errMsg) {
    return extractID(doc[field.name()], field, out, errMsg);
}

FieldParser::FieldState FieldParser::extractID(BSONElement elem,
                                               const BSONField<BSONObj>& field,
                                               BSONObj* out,
                                               string* errMsg) {
    if (elem.eoo()) {
        if (field.hasDefault()) {
            *out = field.getDefault().firstElement().wrap("");
            return FIELD_DEFAULT;
        } else {
            return FIELD_NONE;
        }
    }

    if (elem.type() != BSONType::array) {
        *out = elem.wrap("").getOwned();
        return FIELD_SET;
    }

    _genFieldErrMsg(elem, field, "id", errMsg);
    return FIELD_INVALID;
}

}  // namespace mongo
