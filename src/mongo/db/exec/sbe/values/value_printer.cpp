/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include "mongo/db/exec/sbe/values/value_printer.h"
#include "mongo/db/exec/sbe/values/makeobj_spec.h"
#include "mongo/db/exec/sbe/values/sort_spec.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/platform/basic.h"
#include "mongo/util/pcre_util.h"

namespace mongo::sbe::value {

template <typename T>
ValuePrinter<T>::ValuePrinter(T& stream, const PrintOptions& options)
    : stream(stream), options(options) {}
template <typename T>
void ValuePrinter<T>::writeTagToStream(TypeTags tag) {
    switch (tag) {
        case TypeTags::Nothing:
            stream << "Nothing";
            break;
        case TypeTags::NumberInt32:
            stream << "NumberInt32";
            break;
        case TypeTags::NumberInt64:
            stream << "NumberInt64";
            break;
        case TypeTags::NumberDouble:
            stream << "NumberDouble";
            break;
        case TypeTags::NumberDecimal:
            stream << "NumberDecimal";
            break;
        case TypeTags::Date:
            stream << "Date";
            break;
        case TypeTags::Timestamp:
            stream << "Timestamp";
            break;
        case TypeTags::Boolean:
            stream << "Boolean";
            break;
        case TypeTags::Null:
            stream << "Null";
            break;
        case TypeTags::StringSmall:
            stream << "StringSmall";
            break;
        case TypeTags::StringBig:
            stream << "StringBig";
            break;
        case TypeTags::Array:
            stream << "Array";
            break;
        case TypeTags::ArraySet:
            stream << "ArraySet";
            break;
        case TypeTags::Object:
            stream << "Object";
            break;
        case TypeTags::ObjectId:
            stream << "ObjectId";
            break;
        case TypeTags::MinKey:
            stream << "MinKey";
            break;
        case TypeTags::MaxKey:
            stream << "MaxKey";
            break;
        case TypeTags::bsonObject:
            stream << "bsonObject";
            break;
        case TypeTags::bsonArray:
            stream << "bsonArray";
            break;
        case TypeTags::bsonString:
            stream << "bsonString";
            break;
        case TypeTags::bsonSymbol:
            stream << "bsonSymbol";
            break;
        case TypeTags::bsonObjectId:
            stream << "bsonObjectId";
            break;
        case TypeTags::bsonBinData:
            stream << "bsonBinData";
            break;
        case TypeTags::LocalLambda:
            stream << "LocalLambda";
            break;
        case TypeTags::bsonUndefined:
            stream << "bsonUndefined";
            break;
        case TypeTags::ksValue:
            stream << "KeyString";
            break;
        case TypeTags::pcreRegex:
            stream << "pcreRegex";
            break;
        case TypeTags::timeZoneDB:
            stream << "timeZoneDB";
            break;
        case TypeTags::RecordId:
            stream << "RecordId";
            break;
        case TypeTags::jsFunction:
            stream << "jsFunction";
            break;
        case TypeTags::shardFilterer:
            stream << "shardFilterer";
            break;
        case TypeTags::collator:
            stream << "collator";
            break;
        case TypeTags::bsonRegex:
            stream << "bsonRegex";
            break;
        case TypeTags::bsonJavascript:
            stream << "bsonJavascript";
            break;
        case TypeTags::bsonDBPointer:
            stream << "bsonDBPointer";
            break;
        case TypeTags::bsonCodeWScope:
            stream << "bsonCodeWScope";
            break;
        case TypeTags::ftsMatcher:
            stream << "ftsMatcher";
            break;
        case TypeTags::sortSpec:
            stream << "sortSpec";
            break;
        case TypeTags::makeObjSpec:
            stream << "makeObjSpec";
            break;
        case TypeTags::indexBounds:
            stream << "indexBounds";
            break;
        case TypeTags::classicMatchExpresion:
            stream << "classicMatchExpression";
            break;
        default:
            stream << "unknown tag";
            break;
    }
}

template <typename T>
void ValuePrinter<T>::writeStringDataToStream(StringData sd, bool isJavaScript) {
    if (!isJavaScript) {
        stream << '"';
    }
    if (sd.size() <= options.stringMaxDisplayLength()) {
        stream << sd;
        if (!isJavaScript) {
            stream << '"';
        }
    } else {
        stream << sd.substr(0, options.stringMaxDisplayLength());
        if (!isJavaScript) {
            stream << "\"...";
        } else {
            stream << "...";
        }
    }
}

template <typename T>
void ValuePrinter<T>::writeArrayToStream(TypeTags tag, Value val, size_t depth) {
    stream << '[';
    auto shouldTruncate = true;
    size_t iter = 0;
    if (auto ae = ArrayEnumerator{tag, val}; !ae.atEnd()) {
        while (iter < options.arrayObjectOrNestingMaxDepth() &&
               depth < options.arrayObjectOrNestingMaxDepth()) {
            auto [aeTag, aeVal] = ae.getViewOfValue();
            if (aeTag == TypeTags::Array || aeTag == TypeTags::Object) {
                ++depth;
            }
            writeValueToStream(aeTag, aeVal, depth);
            ae.advance();
            if (ae.atEnd()) {
                shouldTruncate = false;
                break;
            }
            stream << ", ";
            ++iter;
        }
        if (shouldTruncate || depth > options.arrayObjectOrNestingMaxDepth()) {
            stream << "...";
        }
    }
    stream << ']';
}

template <typename T>
void ValuePrinter<T>::writeObjectToStream(TypeTags tag, Value val, size_t depth) {
    stream << '{';
    auto shouldTruncate = true;
    size_t iter = 0;
    if (auto oe = ObjectEnumerator{tag, val}; !oe.atEnd()) {
        while (iter < options.arrayObjectOrNestingMaxDepth() &&
               depth < options.arrayObjectOrNestingMaxDepth()) {
            stream << "\"" << oe.getFieldName() << "\" : ";
            auto [oeTag, oeVal] = oe.getViewOfValue();
            if (oeTag == TypeTags::Array || oeTag == TypeTags::Object) {
                ++depth;
            }
            writeValueToStream(oeTag, oeVal, depth);

            oe.advance();
            if (oe.atEnd()) {
                shouldTruncate = false;
                break;
            }

            stream << ", ";
            ++iter;
        }
        if (shouldTruncate || depth > options.arrayObjectOrNestingMaxDepth()) {
            stream << "...";
        }
    }
    stream << '}';
}

template <typename T>
void ValuePrinter<T>::writeObjectToStream(const BSONObj& obj) {
    writeObjectToStream(TypeTags::bsonObject, bitcastFrom<const char*>(obj.objdata()));
}

template <typename T>
void ValuePrinter<T>::writeObjectIdToStream(TypeTags tag, Value val) {
    auto objId =
        tag == TypeTags::ObjectId ? getObjectIdView(val)->data() : bitcastTo<uint8_t*>(val);

    stream << (tag == TypeTags::ObjectId ? "ObjectId(\"" : "bsonObjectId(\"")
           << OID::from(objId).toString() << "\")";
}

template <typename T>
void ValuePrinter<T>::writeCollatorToStream(const CollatorInterface* collator) {
    if (collator) {
        stream << "Collator(";
        writeObjectToStream(collator->getSpec().toBSON());
        stream << ')';
    } else {
        stream << "null";
    }
}

template <typename T>
void ValuePrinter<T>::writeBsonRegexToStream(const BsonRegex& regex) {
    stream << '/';
    if (regex.pattern.size() <= options.stringMaxDisplayLength()) {
        stream << regex.pattern;
    } else {
        stream << regex.pattern.substr(0, options.stringMaxDisplayLength()) << " ... ";
    }
    stream << '/' << regex.flags;
}

template <typename T>
void ValuePrinter<T>::writeValueToStream(TypeTags tag, Value val, size_t depth) {
    switch (tag) {
        case TypeTags::NumberInt32:
            stream << bitcastTo<int32_t>(val);
            break;
        case TypeTags::NumberInt64:
            stream << bitcastTo<int64_t>(val);
            if (options.useTagForAmbiguousValues()) {
                stream << "ll";
            }
            break;
        case TypeTags::NumberDouble:
            stream << bitcastTo<double>(val);
            if (options.useTagForAmbiguousValues()) {
                stream << "L";
            }
            break;
        case TypeTags::NumberDecimal:
            if (options.useTagForAmbiguousValues()) {
                writeTagToStream(tag);
                stream << "(";
            }
            stream << bitcastTo<Decimal128>(val).toString();
            if (options.useTagForAmbiguousValues()) {
                stream << ")";
            }
            break;
        case TypeTags::Date:
            if (options.useTagForAmbiguousValues()) {
                writeTagToStream(tag);
                stream << "(";
            }
            stream << bitcastTo<int64_t>(val);
            if (options.useTagForAmbiguousValues()) {
                stream << ")";
            }
            break;
        case TypeTags::Boolean:
            stream << (bitcastTo<bool>(val) ? "true" : "false");
            break;
        case TypeTags::Null:
            stream << "null";
            break;
        case TypeTags::StringSmall:
        case TypeTags::StringBig:
        case TypeTags::bsonString:
            writeStringDataToStream(getStringOrSymbolView(tag, val));
            break;
        case TypeTags::bsonSymbol:
            stream << "Symbol(";
            writeStringDataToStream(getStringOrSymbolView(tag, val));
            stream << ')';
            break;
        case TypeTags::Array:
        case TypeTags::ArraySet:
        case TypeTags::bsonArray:
            writeArrayToStream(tag, val, depth);
            break;
        case TypeTags::Object:
        case TypeTags::bsonObject:
            writeObjectToStream(tag, val, depth);
            break;
        case TypeTags::ObjectId:
        case TypeTags::bsonObjectId:
            writeObjectIdToStream(tag, val);
            break;
        case TypeTags::Nothing:
            stream << "Nothing";
            break;
        case TypeTags::MinKey:
            stream << "minKey";
            break;
        case TypeTags::MaxKey:
            stream << "maxKey";
            break;
        case TypeTags::bsonBinData: {
            auto data =
                reinterpret_cast<const char*>(getBSONBinDataCompat(TypeTags::bsonBinData, val));
            auto len = getBSONBinDataSizeCompat(TypeTags::bsonBinData, val);
            auto type = getBSONBinDataSubtype(TypeTags::bsonBinData, val);

            // If the BinData is a correctly sized newUUID, display it as such.
            if (type == newUUID && len == kNewUUIDLength) {
                using namespace fmt::literals;
                StringData sd(data, len);
                // 4 Octets - 2 Octets - 2 Octets - 2 Octets - 6 Octets
                stream << "UUID(\"{}-{}-{}-{}-{}\")"_format(hexblob::encodeLower(sd.substr(0, 4)),
                                                            hexblob::encodeLower(sd.substr(4, 2)),
                                                            hexblob::encodeLower(sd.substr(6, 2)),
                                                            hexblob::encodeLower(sd.substr(8, 2)),
                                                            hexblob::encodeLower(sd.substr(10, 6)));
                break;
            }

            stream << "BinData(" << type << ", "
                   << hexblob::encode(data, std::min(len, options.binDataMaxDisplayLength()))
                   << (len > options.binDataMaxDisplayLength() ? "...)" : ")");
            break;
        }
        case TypeTags::bsonUndefined:
            stream << "undefined";
            break;
        case TypeTags::LocalLambda:
            stream << "LocalLambda";
            break;
        case TypeTags::ksValue: {
            auto ks = getKeyStringView(val);
            stream << "KS(" << ks->toString() << ")";
            break;
        }
        case TypeTags::Timestamp: {
            if (options.useTagForAmbiguousValues()) {
                writeTagToStream(tag);
                stream << "(";
            }
            Timestamp ts{bitcastTo<uint64_t>(val)};
            stream << ts.toString();
            if (options.useTagForAmbiguousValues()) {
                stream << ")";
            }
            break;
        }
        case TypeTags::pcreRegex: {
            auto regex = getPcreRegexView(val);
            stream << "PcreRegex(/" << regex->pattern() << "/"
                   << pcre_util::optionsToFlags(regex->options()) << ")";
            break;
        }
        case TypeTags::timeZoneDB: {
            auto tzdb = getTimeZoneDBView(val);
            auto timeZones = tzdb->getTimeZoneStrings();
            stream << "TimeZoneDatabase(" << timeZones.front() << "..." << timeZones.back() + ")";
            break;
        }
        case TypeTags::RecordId:
            stream << "RecordId(" << getRecordIdView(val)->toString() << ")";
            break;
        case TypeTags::jsFunction:
            // TODO: Also include code.
            stream << "jsFunction";
            break;
        case TypeTags::shardFilterer:
            stream << "ShardFilterer";
            break;
        case TypeTags::collator:
            writeCollatorToStream(getCollatorView(val));
            break;
        case TypeTags::bsonRegex: {
            writeBsonRegexToStream(getBsonRegexView(val));
            break;
        }
        case TypeTags::bsonJavascript:
            stream << "Javascript(";
            writeStringDataToStream(getStringView(TypeTags::StringBig, val), true);
            stream << ")";
            break;
        case TypeTags::bsonDBPointer: {
            const auto dbptr = getBsonDBPointerView(val);
            stream << "DBPointer(";
            writeStringDataToStream(dbptr.ns);
            stream << ", ";
            writeObjectIdToStream(TypeTags::bsonObjectId, bitcastFrom<const uint8_t*>(dbptr.id));
            stream << ')';
            break;
        }
        case TypeTags::bsonCodeWScope: {
            const auto cws = getBsonCodeWScopeView(val);
            stream << "CodeWScope(" << cws.code << ", ";
            writeObjectToStream(TypeTags::bsonObject, bitcastFrom<const char*>(cws.scope));
            stream << ')';
            break;
        }
        case TypeTags::ftsMatcher: {
            auto ftsMatcher = getFtsMatcherView(val);
            stream << "FtsMatcher(";
            writeObjectToStream(ftsMatcher->query().toBSON());
            stream << ')';
            break;
        }
        case TypeTags::sortSpec:
            stream << "SortSpec(";
            writeObjectToStream(getSortSpecView(val)->getPattern());
            stream << ')';
            break;
        case TypeTags::makeObjSpec:
            stream << "MakeObjSpec(" << getMakeObjSpecView(val)->toString() << ")";
            break;
        case TypeTags::indexBounds:
            // When calling toString() we don't know if the index has a non-simple collation or
            // not. Passing false could produce invalid UTF-8, which is not acceptable when we are
            // going to put the resulting string into a BSON object and return it across the wire.
            // While passing true may be misleading in cases when the index has no collation, it is
            // safer to do so.
            stream << "IndexBounds(";
            writeStringDataToStream(
                getIndexBoundsView(val)->toString(true /* hasNonSimpleCollation */));
            stream << ")";
            break;
        case TypeTags::classicMatchExpresion:
            stream << "ClassicMatcher(" << getClassicMatchExpressionView(val)->toString() << ")";
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

template class ValuePrinter<std::ostream>;
template class ValuePrinter<str::stream>;

ValuePrinter<std::ostream> ValuePrinters::make(std::ostream& stream, const PrintOptions& options) {
    return ValuePrinter(stream, options);
}

ValuePrinter<str::stream> ValuePrinters::make(str::stream& stream, const PrintOptions& options) {
    return ValuePrinter(stream, options);
}

}  // namespace mongo::sbe::value
