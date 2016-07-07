/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/bson/bsonelement.h"

#include <boost/functional/hash.hpp>
#include <cmath>

#include "mongo/base/compare_numbers.h"
#include "mongo/base/data_cursor.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/strnlen.h"
#include "mongo/util/base64.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/string_map.h"

namespace mongo {
namespace str = mongoutils::str;

using std::dec;
using std::hex;
using std::string;

string BSONElement::jsonString(JsonStringFormat format, bool includeFieldNames, int pretty) const {
    std::stringstream s;
    if (includeFieldNames)
        s << '"' << escape(fieldName()) << "\" : ";
    switch (type()) {
        case mongo::String:
        case Symbol:
            s << '"' << escape(string(valuestr(), valuestrsize() - 1)) << '"';
            break;
        case NumberLong:
            if (format == TenGen) {
                s << "NumberLong(" << _numberLong() << ")";
            } else {
                s << "{ \"$numberLong\" : \"" << _numberLong() << "\" }";
            }
            break;
        case NumberInt:
            if (format == JS) {
                s << "NumberInt(" << _numberInt() << ")";
                break;
            }
        case NumberDouble:
            if (number() >= -std::numeric_limits<double>::max() &&
                number() <= std::numeric_limits<double>::max()) {
                s.precision(16);
                s << number();
            }
            // This is not valid JSON, but according to RFC-4627, "Numeric values that cannot be
            // represented as sequences of digits (such as Infinity and NaN) are not permitted." so
            // we are accepting the fact that if we have such values we cannot output valid JSON.
            else if (std::isnan(number())) {
                s << "NaN";
            } else if (std::isinf(number())) {
                s << (number() > 0 ? "Infinity" : "-Infinity");
            } else {
                StringBuilder ss;
                ss << "Number " << number() << " cannot be represented in JSON";
                string message = ss.str();
                massert(10311, message.c_str(), false);
            }
            break;
        case NumberDecimal:
            if (format == TenGen)
                s << "NumberDecimal(\"";
            else
                s << "{ \"$numberDecimal\" : \"";
            // Recognize again that this is not valid JSON according to RFC-4627.
            // Also, treat -NaN and +NaN as the same thing for MongoDB.
            if (numberDecimal().isNaN()) {
                s << "NaN";
            } else if (numberDecimal().isInfinite()) {
                s << (numberDecimal().isNegative() ? "-Infinity" : "Infinity");
            } else {
                s << numberDecimal().toString();
            }
            if (format == TenGen)
                s << "\")";
            else
                s << "\" }";
            break;
        case mongo::Bool:
            s << (boolean() ? "true" : "false");
            break;
        case jstNULL:
            s << "null";
            break;
        case Undefined:
            if (format == Strict) {
                s << "{ \"$undefined\" : true }";
            } else {
                s << "undefined";
            }
            break;
        case Object:
            s << embeddedObject().jsonString(format, pretty);
            break;
        case mongo::Array: {
            if (embeddedObject().isEmpty()) {
                s << "[]";
                break;
            }
            s << "[ ";
            BSONObjIterator i(embeddedObject());
            BSONElement e = i.next();
            if (!e.eoo()) {
                int count = 0;
                while (1) {
                    if (pretty) {
                        s << '\n';
                        for (int x = 0; x < pretty; x++)
                            s << "  ";
                    }

                    if (strtol(e.fieldName(), 0, 10) > count) {
                        s << "undefined";
                    } else {
                        s << e.jsonString(format, false, pretty ? pretty + 1 : 0);
                        e = i.next();
                    }
                    count++;
                    if (e.eoo())
                        break;
                    s << ", ";
                }
            }
            s << " ]";
            break;
        }
        case DBRef: {
            if (format == TenGen)
                s << "Dbref( ";
            else
                s << "{ \"$ref\" : ";
            s << '"' << valuestr() << "\", ";
            if (format != TenGen)
                s << "\"$id\" : ";
            s << '"' << mongo::OID::from(valuestr() + valuestrsize()) << "\" ";
            if (format == TenGen)
                s << ')';
            else
                s << '}';
            break;
        }
        case jstOID:
            if (format == TenGen) {
                s << "ObjectId( ";
            } else {
                s << "{ \"$oid\" : ";
            }
            s << '"' << __oid() << '"';
            if (format == TenGen) {
                s << " )";
            } else {
                s << " }";
            }
            break;
        case BinData: {
            ConstDataCursor reader(value());
            const int len = reader.readAndAdvance<LittleEndian<int>>();
            BinDataType type = static_cast<BinDataType>(reader.readAndAdvance<uint8_t>());

            s << "{ \"$binary\" : \"";
            base64::encode(s, reader.view(), len);
            s << "\", \"$type\" : \"" << hex;
            s.width(2);
            s.fill('0');
            s << type << dec;
            s << "\" }";
            break;
        }
        case mongo::Date:
            if (format == Strict) {
                Date_t d = date();
                s << "{ \"$date\" : ";
                // The two cases in which we cannot convert Date_t::millis to an ISO Date string are
                // when the date is too large to format (SERVER-13760), and when the date is before
                // the epoch (SERVER-11273).  Since Date_t internally stores millis as an unsigned
                // long long, despite the fact that it is logically signed (SERVER-8573), this check
                // handles both the case where Date_t::millis is too large, and the case where
                // Date_t::millis is negative (before the epoch).
                if (d.isFormattable()) {
                    s << "\"" << dateToISOStringLocal(date()) << "\"";
                } else {
                    s << "{ \"$numberLong\" : \"" << d.toMillisSinceEpoch() << "\" }";
                }
                s << " }";
            } else {
                s << "Date( ";
                if (pretty) {
                    Date_t d = date();
                    // The two cases in which we cannot convert Date_t::millis to an ISO Date string
                    // are when the date is too large to format (SERVER-13760), and when the date is
                    // before the epoch (SERVER-11273).  Since Date_t internally stores millis as an
                    // unsigned long long, despite the fact that it is logically signed
                    // (SERVER-8573), this check handles both the case where Date_t::millis is too
                    // large, and the case where Date_t::millis is negative (before the epoch).
                    if (d.isFormattable()) {
                        s << "\"" << dateToISOStringLocal(date()) << "\"";
                    } else {
                        // FIXME: This is not parseable by the shell, since it may not fit in a
                        // float
                        s << d.toMillisSinceEpoch();
                    }
                } else {
                    s << date().asInt64();
                }
                s << " )";
            }
            break;
        case RegEx:
            if (format == Strict) {
                s << "{ \"$regex\" : \"" << escape(regex());
                s << "\", \"$options\" : \"" << regexFlags() << "\" }";
            } else {
                s << "/" << escape(regex(), true) << "/";
                // FIXME Worry about alpha order?
                for (const char* f = regexFlags(); *f; ++f) {
                    switch (*f) {
                        case 'g':
                        case 'i':
                        case 'm':
                            s << *f;
                        default:
                            break;
                    }
                }
            }
            break;

        case CodeWScope: {
            BSONObj scope = codeWScopeObject();
            if (!scope.isEmpty()) {
                s << "{ \"$code\" : \"" << escape(_asCode()) << "\" , "
                  << "\"$scope\" : " << scope.jsonString() << " }";
                break;
            }
        }

        case Code:
            s << "\"" << escape(_asCode()) << "\"";
            break;

        case bsonTimestamp:
            if (format == TenGen) {
                s << "Timestamp( " << durationCount<Seconds>(timestampTime().toDurationSinceEpoch())
                  << ", " << timestampInc() << " )";
            } else {
                s << "{ \"$timestamp\" : { \"t\" : "
                  << durationCount<Seconds>(timestampTime().toDurationSinceEpoch())
                  << ", \"i\" : " << timestampInc() << " } }";
            }
            break;

        case MinKey:
            s << "{ \"$minKey\" : 1 }";
            break;

        case MaxKey:
            s << "{ \"$maxKey\" : 1 }";
            break;

        default:
            StringBuilder ss;
            ss << "Cannot create a properly formatted JSON string with "
               << "element: " << toString() << " of type: " << type();
            string message = ss.str();
            massert(10312, message.c_str(), false);
    }
    return s.str();
}

namespace {

// Map from query operator string name to operator MatchType. Used in BSONElement::getGtLtOp().
const StringMap<BSONObj::MatchType> queryOperatorMap{
    // TODO: SERVER-19565 Add $eq after auditing callers.
    {"lt", BSONObj::LT},
    {"lte", BSONObj::LTE},
    {"gte", BSONObj::GTE},
    {"gt", BSONObj::GT},
    {"in", BSONObj::opIN},
    {"ne", BSONObj::NE},
    {"size", BSONObj::opSIZE},
    {"all", BSONObj::opALL},
    {"nin", BSONObj::NIN},
    {"exists", BSONObj::opEXISTS},
    {"mod", BSONObj::opMOD},
    {"type", BSONObj::opTYPE},
    {"regex", BSONObj::opREGEX},
    {"options", BSONObj::opOPTIONS},
    {"elemMatch", BSONObj::opELEM_MATCH},
    {"near", BSONObj::opNEAR},
    {"nearSphere", BSONObj::opNEAR},
    {"geoNear", BSONObj::opNEAR},
    {"within", BSONObj::opWITHIN},
    {"geoWithin", BSONObj::opWITHIN},
    {"geoIntersects", BSONObj::opGEO_INTERSECTS},
    {"bitsAllSet", BSONObj::opBITS_ALL_SET},
    {"bitsAllClear", BSONObj::opBITS_ALL_CLEAR},
    {"bitsAnySet", BSONObj::opBITS_ANY_SET},
    {"bitsAnyClear", BSONObj::opBITS_ANY_CLEAR},
};

}  // namespace

int BSONElement::getGtLtOp(int def) const {
    const char* fn = fieldName();
    if (fn[0] == '$' && fn[1]) {
        StringData opName = fieldNameStringData().substr(1);

        StringMap<BSONObj::MatchType>::const_iterator queryOp = queryOperatorMap.find(opName);
        if (queryOp == queryOperatorMap.end()) {
            return def;
        }
        return queryOp->second;
    }
    return def;
}

/** transform a BSON array into a vector of BSONElements.
    we match array # positions with their vector position, and ignore
    any fields with non-numeric field names.
    */
std::vector<BSONElement> BSONElement::Array() const {
    chk(mongo::Array);
    std::vector<BSONElement> v;
    BSONObjIterator i(Obj());
    while (i.more()) {
        BSONElement e = i.next();
        const char* f = e.fieldName();

        unsigned u;
        Status status = parseNumberFromString(f, &u);
        if (status.isOK()) {
            verify(u < 1000000);
            if (u >= v.size())
                v.resize(u + 1);
            v[u] = e;
        } else {
            // ignore?
        }
    }
    return v;
}

/* wo = "well ordered"
   note: (mongodb related) : this can only change in behavior when index version # changes
*/
int BSONElement::woCompare(const BSONElement& e,
                           bool considerFieldName,
                           const StringData::ComparatorInterface* comparator) const {
    int lt = (int)canonicalType();
    int rt = (int)e.canonicalType();
    int x = lt - rt;
    if (x != 0 && (!isNumber() || !e.isNumber()))
        return x;
    if (considerFieldName) {
        x = strcmp(fieldName(), e.fieldName());
        if (x != 0)
            return x;
    }
    x = compareElementValues(*this, e, comparator);
    return x;
}

bool BSONElement::binaryEqual(const BSONElement& rhs) const {
    const int elemSize = size();

    if (elemSize != rhs.size()) {
        return false;
    }

    return (elemSize == 0) || (memcmp(data, rhs.rawdata(), elemSize) == 0);
}

bool BSONElement::binaryEqualValues(const BSONElement& rhs) const {
    // The binaryEqual method above implicitly compares the type, but we need to do so explicitly
    // here. It doesn't make sense to consider to BSONElement objects as binaryEqual if they have
    // the same bit pattern but different types (consider an integer and a double).
    if (type() != rhs.type())
        return false;

    const int valueSize = valuesize();
    if (valueSize != rhs.valuesize()) {
        return false;
    }

    return (valueSize == 0) || (memcmp(value(), rhs.value(), valueSize) == 0);
}

BSONObj BSONElement::embeddedObjectUserCheck() const {
    if (MONGO_likely(isABSONObj()))
        return BSONObj(value());
    std::stringstream ss;
    ss << "invalid parameter: expected an object (" << fieldName() << ")";
    uasserted(10065, ss.str());
    return BSONObj();  // never reachable
}

BSONObj BSONElement::embeddedObject() const {
    verify(isABSONObj());
    return BSONObj(value());
}

BSONObj BSONElement::codeWScopeObject() const {
    verify(type() == CodeWScope);
    int strSizeWNull = ConstDataView(value() + 4).read<LittleEndian<int>>();
    return BSONObj(value() + 4 + 4 + strSizeWNull);
}

// wrap this element up as a singleton object.
BSONObj BSONElement::wrap() const {
    BSONObjBuilder b(size() + 6);
    b.append(*this);
    return b.obj();
}

BSONObj BSONElement::wrap(StringData newName) const {
    BSONObjBuilder b(size() + 6 + newName.size());
    b.appendAs(*this, newName);
    return b.obj();
}

void BSONElement::Val(BSONObj& v) const {
    v = Obj();
}

BSONObj BSONElement::Obj() const {
    return embeddedObjectUserCheck();
}

BSONElement BSONElement::operator[](const std::string& field) const {
    BSONObj o = Obj();
    return o[field];
}

int BSONElement::size(int maxLen) const {
    if (totalSize >= 0)
        return totalSize;

    int remain = maxLen - fieldNameSize() - 1;

    int x = 0;
    switch (type()) {
        case EOO:
        case Undefined:
        case jstNULL:
        case MaxKey:
        case MinKey:
            break;
        case mongo::Bool:
            x = 1;
            break;
        case NumberInt:
            x = 4;
            break;
        case bsonTimestamp:
        case mongo::Date:
        case NumberDouble:
        case NumberLong:
            x = 8;
            break;
        case NumberDecimal:
            x = 16;
            break;
        case jstOID:
            x = OID::kOIDSize;
            break;
        case Symbol:
        case Code:
        case mongo::String:
            massert(
                10313, "Insufficient bytes to calculate element size", maxLen == -1 || remain > 3);
            x = valuestrsize() + 4;
            break;
        case CodeWScope:
            massert(
                10314, "Insufficient bytes to calculate element size", maxLen == -1 || remain > 3);
            x = objsize();
            break;

        case DBRef:
            massert(
                10315, "Insufficient bytes to calculate element size", maxLen == -1 || remain > 3);
            x = valuestrsize() + 4 + 12;
            break;
        case Object:
        case mongo::Array:
            massert(
                10316, "Insufficient bytes to calculate element size", maxLen == -1 || remain > 3);
            x = objsize();
            break;
        case BinData:
            massert(
                10317, "Insufficient bytes to calculate element size", maxLen == -1 || remain > 3);
            x = valuestrsize() + 4 + 1 /*subtype*/;
            break;
        case RegEx: {
            const char* p = value();
            size_t len1 = (maxLen == -1) ? strlen(p) : strnlen(p, remain);
            massert(10318, "Invalid regex string", maxLen == -1 || len1 < size_t(remain));
            p = p + len1 + 1;
            size_t len2;
            if (maxLen == -1)
                len2 = strlen(p);
            else {
                size_t x = remain - len1 - 1;
                verify(x <= 0x7fffffff);
                len2 = strnlen(p, x);
                massert(10319, "Invalid regex options string", len2 < x);
            }
            x = (int)(len1 + 1 + len2 + 1);
        } break;
        default: {
            StringBuilder ss;
            ss << "BSONElement: bad type " << (int)type();
            std::string msg = ss.str();
            massert(13655, msg.c_str(), false);
        }
    }
    totalSize = x + fieldNameSize() + 1;  // BSONType

    return totalSize;
}

int BSONElement::size() const {
    if (totalSize >= 0)
        return totalSize;

    int x = 0;
    switch (type()) {
        case EOO:
        case Undefined:
        case jstNULL:
        case MaxKey:
        case MinKey:
            break;
        case mongo::Bool:
            x = 1;
            break;
        case NumberInt:
            x = 4;
            break;
        case bsonTimestamp:
        case mongo::Date:
        case NumberDouble:
        case NumberLong:
            x = 8;
            break;
        case NumberDecimal:
            x = 16;
            break;
        case jstOID:
            x = OID::kOIDSize;
            break;
        case Symbol:
        case Code:
        case mongo::String:
            x = valuestrsize() + 4;
            break;
        case DBRef:
            x = valuestrsize() + 4 + 12;
            break;
        case CodeWScope:
        case Object:
        case mongo::Array:
            x = objsize();
            break;
        case BinData:
            x = valuestrsize() + 4 + 1 /*subtype*/;
            break;
        case RegEx: {
            const char* p = value();
            size_t len1 = strlen(p);
            p = p + len1 + 1;
            size_t len2;
            len2 = strlen(p);
            x = (int)(len1 + 1 + len2 + 1);
        } break;
        default: {
            StringBuilder ss;
            ss << "BSONElement: bad type " << (int)type();
            std::string msg = ss.str();
            massert(10320, msg.c_str(), false);
        }
    }
    totalSize = x + fieldNameSize() + 1;  // BSONType

    return totalSize;
}

std::string BSONElement::toString(bool includeFieldName, bool full) const {
    StringBuilder s;
    toString(s, includeFieldName, full, false);
    return s.str();
}

void BSONElement::toString(
    StringBuilder& s, bool includeFieldName, bool full, bool redactValues, int depth) const {
    if (depth > BSONObj::maxToStringRecursionDepth) {
        // check if we want the full/complete string
        if (full) {
            StringBuilder s;
            s << "Reached maximum recursion depth of ";
            s << BSONObj::maxToStringRecursionDepth;
            uassert(16150, s.str(), full != true);
        }
        s << "...";
        return;
    }

    if (includeFieldName && type() != EOO)
        s << fieldName() << ": ";

    switch (type()) {
        case Object:
            return embeddedObject().toString(s, false, full, redactValues, depth + 1);
        case mongo::Array:
            return embeddedObject().toString(s, true, full, redactValues, depth + 1);
        default:
            break;
    }

    if (redactValues) {
        s << "\"###\"";
        return;
    }

    switch (type()) {
        case EOO:
            s << "EOO";
            break;
        case mongo::Date:
            s << "new Date(" << date().toMillisSinceEpoch() << ')';
            break;
        case RegEx: {
            s << "/" << regex() << '/';
            const char* p = regexFlags();
            if (p)
                s << p;
        } break;
        case NumberDouble:
            s.appendDoubleNice(number());
            break;
        case NumberLong:
            s << _numberLong();
            break;
        case NumberInt:
            s << _numberInt();
            break;
        case NumberDecimal:
            s << _numberDecimal().toString();
            break;
        case mongo::Bool:
            s << (boolean() ? "true" : "false");
            break;
        case Undefined:
            s << "undefined";
            break;
        case jstNULL:
            s << "null";
            break;
        case MaxKey:
            s << "MaxKey";
            break;
        case MinKey:
            s << "MinKey";
            break;
        case CodeWScope:
            s << "CodeWScope( " << codeWScopeCode() << ", " << codeWScopeObject().toString() << ")";
            break;
        case Code:
            if (!full && valuestrsize() > 80) {
                s.write(valuestr(), 70);
                s << "...";
            } else {
                s.write(valuestr(), valuestrsize() - 1);
            }
            break;
        case Symbol:
        case mongo::String:
            s << '"';
            if (!full && valuestrsize() > 160) {
                s.write(valuestr(), 150);
                s << "...\"";
            } else {
                s.write(valuestr(), valuestrsize() - 1);
                s << '"';
            }
            break;
        case DBRef:
            s << "DBRef('" << valuestr() << "',";
            s << mongo::OID::from(valuestr() + valuestrsize()) << ')';
            break;
        case jstOID:
            s << "ObjectId('";
            s << __oid() << "')";
            break;
        case BinData:
            s << "BinData(" << binDataType() << ", ";
            {
                int len;
                const char* data = binDataClean(len);
                if (!full && len > 80) {
                    s << toHex(data, 70) << "...)";
                } else {
                    s << toHex(data, len) << ")";
                }
            }
            break;
        case bsonTimestamp:
            s << "Timestamp " << timestampTime().toMillisSinceEpoch() << "|" << timestampInc();
            break;
        default:
            s << "?type=" << type();
            break;
    }
}

std::string BSONElement::_asCode() const {
    switch (type()) {
        case mongo::String:
        case Code:
            return std::string(valuestr(), valuestrsize() - 1);
        case CodeWScope:
            return std::string(codeWScopeCode(),
                               ConstDataView(valuestr()).read<LittleEndian<int>>() - 1);
        default:
            log() << "can't convert type: " << (int)(type()) << " to code" << std::endl;
    }
    uassert(10062, "not code", 0);
    return "";
}

std::ostream& operator<<(std::ostream& s, const BSONElement& e) {
    return s << e.toString();
}

StringBuilder& operator<<(StringBuilder& s, const BSONElement& e) {
    e.toString(s);
    return s;
}

template <>
bool BSONElement::coerce<std::string>(std::string* out) const {
    if (type() != mongo::String)
        return false;
    *out = String();
    return true;
}

template <>
bool BSONElement::coerce<int>(int* out) const {
    if (!isNumber())
        return false;
    *out = numberInt();
    return true;
}

template <>
bool BSONElement::coerce<long long>(long long* out) const {
    if (!isNumber())
        return false;
    *out = numberLong();
    return true;
}

template <>
bool BSONElement::coerce<double>(double* out) const {
    if (!isNumber())
        return false;
    *out = numberDouble();
    return true;
}

template <>
bool BSONElement::coerce<Decimal128>(Decimal128* out) const {
    if (!isNumber())
        return false;
    *out = numberDecimal();
    return true;
}

template <>
bool BSONElement::coerce<bool>(bool* out) const {
    *out = trueValue();
    return true;
}

template <>
bool BSONElement::coerce<std::vector<std::string>>(std::vector<std::string>* out) const {
    if (type() != mongo::Array)
        return false;
    return Obj().coerceVector<std::string>(out);
}

template <typename T>
bool BSONObj::coerceVector(std::vector<T>* out) const {
    BSONObjIterator i(*this);
    while (i.more()) {
        BSONElement e = i.next();
        T t;
        if (!e.coerce<T>(&t))
            return false;
        out->push_back(t);
    }
    return true;
}

// used by jsonString()
std::string escape(const std::string& s, bool escape_slash) {
    StringBuilder ret;
    for (std::string::const_iterator i = s.begin(); i != s.end(); ++i) {
        switch (*i) {
            case '"':
                ret << "\\\"";
                break;
            case '\\':
                ret << "\\\\";
                break;
            case '/':
                ret << (escape_slash ? "\\/" : "/");
                break;
            case '\b':
                ret << "\\b";
                break;
            case '\f':
                ret << "\\f";
                break;
            case '\n':
                ret << "\\n";
                break;
            case '\r':
                ret << "\\r";
                break;
            case '\t':
                ret << "\\t";
                break;
            default:
                if (*i >= 0 && *i <= 0x1f) {
                    // TODO: these should be utf16 code-units not bytes
                    char c = *i;
                    ret << "\\u00" << toHexLower(&c, 1);
                } else {
                    ret << *i;
                }
        }
    }
    return ret.str();
}

/**
 * l and r must be same canonicalType when called.
 */
int compareElementValues(const BSONElement& l,
                         const BSONElement& r,
                         const StringData::ComparatorInterface* comparator) {
    int f;

    switch (l.type()) {
        case EOO:
        case Undefined:  // EOO and Undefined are same canonicalType
        case jstNULL:
        case MaxKey:
        case MinKey:
            f = l.canonicalType() - r.canonicalType();
            if (f < 0)
                return -1;
            return f == 0 ? 0 : 1;
        case Bool:
            return *l.value() - *r.value();
        case bsonTimestamp:
            // unsigned compare for timestamps - note they are not really dates but (ordinal +
            // time_t)
            if (l.timestamp() < r.timestamp())
                return -1;
            return l.timestamp() == r.timestamp() ? 0 : 1;
        case Date:
            // Signed comparisons for Dates.
            {
                const Date_t a = l.Date();
                const Date_t b = r.Date();
                if (a < b)
                    return -1;
                return a == b ? 0 : 1;
            }

        case NumberInt: {
            // All types can precisely represent all NumberInts, so it is safe to simply convert to
            // whatever rhs's type is.
            switch (r.type()) {
                case NumberInt:
                    return compareInts(l._numberInt(), r._numberInt());
                case NumberLong:
                    return compareLongs(l._numberInt(), r._numberLong());
                case NumberDouble:
                    return compareDoubles(l._numberInt(), r._numberDouble());
                case NumberDecimal:
                    return compareIntToDecimal(l._numberInt(), r._numberDecimal());
                default:
                    invariant(false);
            }
        }

        case NumberLong: {
            switch (r.type()) {
                case NumberLong:
                    return compareLongs(l._numberLong(), r._numberLong());
                case NumberInt:
                    return compareLongs(l._numberLong(), r._numberInt());
                case NumberDouble:
                    return compareLongToDouble(l._numberLong(), r._numberDouble());
                case NumberDecimal:
                    return compareLongToDecimal(l._numberLong(), r._numberDecimal());
                default:
                    invariant(false);
            }
        }

        case NumberDouble: {
            switch (r.type()) {
                case NumberDouble:
                    return compareDoubles(l._numberDouble(), r._numberDouble());
                case NumberInt:
                    return compareDoubles(l._numberDouble(), r._numberInt());
                case NumberLong:
                    return compareDoubleToLong(l._numberDouble(), r._numberLong());
                case NumberDecimal:
                    return compareDoubleToDecimal(l._numberDouble(), r._numberDecimal());
                default:
                    invariant(false);
            }
        }

        case NumberDecimal: {
            switch (r.type()) {
                case NumberDecimal:
                    return compareDecimals(l._numberDecimal(), r._numberDecimal());
                case NumberInt:
                    return compareDecimalToInt(l._numberDecimal(), r._numberInt());
                case NumberLong:
                    return compareDecimalToLong(l._numberDecimal(), r._numberLong());
                case NumberDouble:
                    return compareDecimalToDouble(l._numberDecimal(), r._numberDouble());
                default:
                    invariant(false);
            }
        }

        case jstOID:
            return memcmp(l.value(), r.value(), OID::kOIDSize);
        case Code:
        case Symbol:
        case String: {
            if (comparator) {
                return comparator->compare(l.valueStringData(), r.valueStringData());
            } else {
                // we use memcmp as we allow zeros in UTF8 strings
                int lsz = l.valuestrsize();
                int rsz = r.valuestrsize();
                int common = std::min(lsz, rsz);
                int res = memcmp(l.valuestr(), r.valuestr(), common);
                if (res)
                    return res;
                // longer std::string is the greater one
                return lsz - rsz;
            }
        }
        case Object:
        case Array:
            // woCompare parameters: r, ordering, considerFieldName, comparator.
            // r: the BSONObj to compare with.
            // ordering: the sort directions for each key.
            // considerFieldName: whether field names should be considered in comparison.
            // comparator: used for all string comparisons, if non-null.
            return l.embeddedObject().woCompare(r.embeddedObject(), BSONObj(), true, comparator);
        case DBRef: {
            int lsz = l.valuesize();
            int rsz = r.valuesize();
            if (lsz - rsz != 0)
                return lsz - rsz;
            return memcmp(l.value(), r.value(), lsz);
        }
        case BinData: {
            int lsz = l.objsize();  // our bin data size in bytes, not including the subtype byte
            int rsz = r.objsize();
            if (lsz - rsz != 0)
                return lsz - rsz;
            return memcmp(l.value() + 4, r.value() + 4, lsz + 1 /*+1 for subtype byte*/);
        }
        case RegEx: {
            int c = strcmp(l.regex(), r.regex());
            if (c)
                return c;
            return strcmp(l.regexFlags(), r.regexFlags());
        }
        case CodeWScope: {
            int cmp = StringData(l.codeWScopeCode(), l.codeWScopeCodeLen() - 1)
                          .compare(StringData(r.codeWScopeCode(), r.codeWScopeCodeLen() - 1));
            if (cmp)
                return cmp;

            return l.codeWScopeObject().woCompare(
                // woCompare parameters: r, ordering, considerFieldName, comparator.
                // r: the BSONObj to compare with.
                // ordering: the sort directions for each key.
                // considerFieldName: whether field names should be considered in comparison.
                // comparator: used for all string comparisons, if non-null.
                r.codeWScopeObject(),
                BSONObj(),
                true,
                comparator);
        }
        default:
            verify(false);
    }
    return -1;
}

size_t BSONElement::Hasher::operator()(const BSONElement& elem) const {
    size_t hash = 0;

    boost::hash_combine(hash, elem.canonicalType());

    const StringData fieldName = elem.fieldNameStringData();
    if (!fieldName.empty()) {
        boost::hash_combine(hash, StringData::Hasher()(fieldName));
    }

    switch (elem.type()) {
        // Order of types is the same as in compareElementValues().

        case mongo::EOO:
        case mongo::Undefined:
        case mongo::jstNULL:
        case mongo::MaxKey:
        case mongo::MinKey:
            // These are valueless types
            break;

        case mongo::Bool:
            boost::hash_combine(hash, elem.boolean());
            break;

        case mongo::bsonTimestamp:
            boost::hash_combine(hash, elem.timestamp().asULL());
            break;

        case mongo::Date:
            boost::hash_combine(hash, elem.date().asInt64());
            break;

        case mongo::NumberDecimal: {
            const Decimal128 dcml = elem.numberDecimal();
            if (dcml.toAbs().isGreater(Decimal128(std::numeric_limits<double>::max(),
                                                  Decimal128::kRoundTo34Digits,
                                                  Decimal128::kRoundTowardZero)) &&
                !dcml.isInfinite() && !dcml.isNaN()) {
                // Normalize our decimal to force equivalent decimals
                // in the same cohort to hash to the same value
                Decimal128 dcmlNorm(dcml.normalize());
                boost::hash_combine(hash, dcmlNorm.getValue().low64);
                boost::hash_combine(hash, dcmlNorm.getValue().high64);
                break;
            }
            // Else, fall through and convert the decimal to a double and hash.
            // At this point the decimal fits into the range of doubles, is infinity, or is NaN,
            // which doubles have a cheaper representation for.
        }
        case mongo::NumberDouble:
        case mongo::NumberLong:
        case mongo::NumberInt: {
            // This converts all numbers to doubles, which ignores the low-order bits of
            // NumberLongs > 2**53 and precise decimal numbers without double representations,
            // but that is ok since the hash will still be the same for equal numbers and is
            // still likely to be different for different numbers. (Note: this issue only
            // applies for decimals when they are outside of the valid double range. See
            // the above case.)
            // SERVER-16851
            const double dbl = elem.numberDouble();
            if (std::isnan(dbl)) {
                boost::hash_combine(hash, std::numeric_limits<double>::quiet_NaN());
            } else {
                boost::hash_combine(hash, dbl);
            }
            break;
        }

        case mongo::jstOID:
            elem.__oid().hash_combine(hash);
            break;

        case mongo::Code:
        case mongo::Symbol:
        case mongo::String:
            boost::hash_combine(hash, StringData::Hasher()(elem.valueStringData()));
            break;

        case mongo::Object:
        case mongo::Array:
            boost::hash_combine(hash, BSONObj::Hasher()(elem.embeddedObject()));
            break;

        case mongo::DBRef:
        case mongo::BinData:
            // All bytes of the value are required to be identical.
            boost::hash_combine(hash,
                                StringData::Hasher()(StringData(elem.value(), elem.valuesize())));
            break;

        case mongo::RegEx:
            boost::hash_combine(hash, StringData::Hasher()(elem.regex()));
            boost::hash_combine(hash, StringData::Hasher()(elem.regexFlags()));
            break;

        case mongo::CodeWScope: {
            boost::hash_combine(
                hash,
                StringData::Hasher()(StringData(elem.codeWScopeCode(), elem.codeWScopeCodeLen())));
            boost::hash_combine(hash, BSONObj::Hasher()(elem.codeWScopeObject()));
            break;
        }
    }
    return hash;
}

}  // namespace mongo
