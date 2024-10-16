/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/vm/vm.h"

namespace mongo {
namespace sbe {
namespace vm {
FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericNewKeyString(
    ArityType arity, CollatorInterface* collator) {
    auto [_, tagVersion, valVersion] = getFromStack(0);
    auto [__, tagOrdering, valOrdering] = getFromStack(1);
    auto [___, tagDiscriminator, valDiscriminator] = getFromStack(arity - 1u);
    if (!value::isNumber(tagVersion) || !value::isNumber(tagOrdering) ||
        !value::isNumber(tagDiscriminator)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto version = value::numericCast<int64_t>(tagVersion, valVersion);
    auto discriminator = value::numericCast<int64_t>(tagDiscriminator, valDiscriminator);
    if ((version < 0 || version > 1) || (discriminator < 0 || discriminator > 2)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto ksVersion = static_cast<key_string::Version>(version);
    auto ksDiscriminator = static_cast<key_string::Discriminator>(discriminator);

    uint32_t orderingBits = value::numericCast<int32_t>(tagOrdering, valOrdering);
    BSONObjBuilder bb;
    for (size_t i = 0; orderingBits != 0 && i < arity - 3u; ++i, orderingBits >>= 1) {
        bb.append(""_sd, (orderingBits & 1) ? -1 : 1);
    }

    key_string::HeapBuilder kb{ksVersion, Ordering::make(bb.done())};

    const auto stringTransformFn = [&](StringData stringData) {
        return collator->getComparisonString(stringData);
    };

    for (size_t idx = 2; idx < arity - 1u; ++idx) {
        auto [_, tag, val] = getFromStack(idx);
        // This is needed so that we can use 'tag' in the uassert() below without getting a
        // "Reference to local binding declared in enclosing function" compile error on clang.
        auto tagCopy = tag;

        switch (tag) {
            case value::TypeTags::Boolean:
                kb.appendBool(value::bitcastTo<bool>(val));
                break;
            case value::TypeTags::NumberInt32:
                kb.appendNumberInt(value::bitcastTo<int32_t>(val));
                break;
            case value::TypeTags::NumberInt64:
                kb.appendNumberLong(value::bitcastTo<int64_t>(val));
                break;
            case value::TypeTags::NumberDouble:
                kb.appendNumberDouble(value::bitcastTo<double>(val));
                break;
            case value::TypeTags::NumberDecimal:
                kb.appendNumberDecimal(value::bitcastTo<Decimal128>(val));
                break;
            case value::TypeTags::StringSmall:
            case value::TypeTags::StringBig:
            case value::TypeTags::bsonString:
                if (collator) {
                    kb.appendString(value::getStringView(tag, val), stringTransformFn);
                } else {
                    kb.appendString(value::getStringView(tag, val));
                }
                break;
            case value::TypeTags::Null:
                kb.appendNull();
                break;
            case value::TypeTags::bsonUndefined:
                kb.appendUndefined();
                break;
            case value::TypeTags::bsonJavascript:
                kb.appendCode(value::getBsonJavascriptView(val));
                break;
            case value::TypeTags::Date: {
                auto milliseconds = value::bitcastTo<int64_t>(val);
                auto duration = stdx::chrono::duration<int64_t, std::milli>(milliseconds);
                auto date = Date_t::fromDurationSinceEpoch(duration);
                kb.appendDate(date);
                break;
            }
            case value::TypeTags::Timestamp: {
                Timestamp ts{value::bitcastTo<uint64_t>(val)};
                kb.appendTimestamp(ts);
                break;
            }
            case value::TypeTags::MinKey: {
                BSONObjBuilder bob;
                bob.appendMinKey("");
                kb.appendBSONElement(bob.obj().firstElement());
                break;
            }
            case value::TypeTags::MaxKey: {
                BSONObjBuilder bob;
                bob.appendMaxKey("");
                kb.appendBSONElement(bob.obj().firstElement());
                break;
            }
            case value::TypeTags::bsonArray: {
                BSONObj bson{value::getRawPointerView(val)};
                if (collator) {
                    kb.appendArray(BSONArray(BSONObj(bson)), stringTransformFn);
                } else {
                    kb.appendArray(BSONArray(BSONObj(bson)));
                }
                break;
            }
            case value::TypeTags::Array:
            case value::TypeTags::ArraySet:
            case value::TypeTags::ArrayMultiSet: {
                value::ArrayEnumerator enumerator{tag, val};
                BSONArrayBuilder arrayBuilder;
                bson::convertToBsonArr(arrayBuilder, enumerator);
                if (collator) {
                    kb.appendArray(arrayBuilder.arr(), stringTransformFn);
                } else {
                    kb.appendArray(arrayBuilder.arr());
                }
                break;
            }
            case value::TypeTags::bsonObject: {
                BSONObj bson{value::getRawPointerView(val)};
                if (collator) {
                    kb.appendObject(bson, stringTransformFn);
                } else {
                    kb.appendObject(bson);
                }
                break;
            }
            case value::TypeTags::Object: {
                BSONObjBuilder objBuilder;
                bson::convertToBsonObj(objBuilder, value::getObjectView(val));
                if (collator) {
                    kb.appendObject(objBuilder.obj(), stringTransformFn);
                } else {
                    kb.appendObject(objBuilder.obj());
                }
                break;
            }
            case value::TypeTags::ObjectId: {
                auto oid = OID::from(value::getObjectIdView(val)->data());
                kb.appendOID(oid);
                break;
            }
            case value::TypeTags::bsonObjectId: {
                auto oid = OID::from(value::getRawPointerView(val));
                kb.appendOID(oid);
                break;
            }
            case value::TypeTags::bsonSymbol: {
                auto symbolView = value::getStringOrSymbolView(tag, val);
                kb.appendSymbol(symbolView);
                break;
            }
            case value::TypeTags::bsonBinData: {
                auto data = value::getBSONBinData(tag, val);
                auto length = static_cast<int>(value::getBSONBinDataSize(tag, val));
                auto type = value::getBSONBinDataSubtype(tag, val);
                BSONBinData binData{data, length, type};
                kb.appendBinData(binData);
                break;
            }
            case value::TypeTags::bsonRegex: {
                auto sbeRegex = value::getBsonRegexView(val);
                BSONRegEx regex{sbeRegex.pattern, sbeRegex.flags};
                kb.appendRegex(regex);
                break;
            }
            case value::TypeTags::bsonCodeWScope: {
                auto sbeCodeWScope = value::getBsonCodeWScopeView(val);
                BSONCodeWScope codeWScope{sbeCodeWScope.code, BSONObj(sbeCodeWScope.scope)};
                kb.appendCodeWString(codeWScope);
                break;
            }
            case value::TypeTags::bsonDBPointer: {
                auto dbPointer = value::getBsonDBPointerView(val);
                BSONDBRef dbRef{dbPointer.ns, OID::from(dbPointer.id)};
                kb.appendDBRef(dbRef);
                break;
            }
            default:
                uasserted(4822802, str::stream() << "Unsuppored key string type: " << tagCopy);
                break;
        }
    }

    kb.appendDiscriminator(ksDiscriminator);

    return {true, value::TypeTags::keyString, value::makeKeyString(kb.release()).second};
}  // genericNewKeyString

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinNewKeyString(ArityType arity) {
    tassert(6333000,
            str::stream() << "Unsupported number of arguments passed to ks(): " << arity,
            arity >= 3 && arity <= Ordering::kMaxCompoundIndexKeys + 3);
    return genericNewKeyString(arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCollNewKeyString(ArityType arity) {
    tassert(6511500,
            str::stream() << "Unsupported number of arguments passed to collKs(): " << arity,
            arity >= 4 && arity <= Ordering::kMaxCompoundIndexKeys + 4);

    auto [_, tagCollator, valCollator] = getFromStack(arity - 1u);
    if (tagCollator != value::TypeTags::collator) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto collator = value::getCollatorView(valCollator);
    return genericNewKeyString(arity - 1u, collator);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinKeyStringToString(ArityType arity) {
    auto [owned, tagInKey, valInKey] = getFromStack(0);

    // We operate only on keys.
    if (tagInKey != value::TypeTags::keyString) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto key = value::getKeyString(valInKey);

    auto [tagStr, valStr] = value::makeNewString(key->toString());

    return {true, tagStr, valStr};
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
