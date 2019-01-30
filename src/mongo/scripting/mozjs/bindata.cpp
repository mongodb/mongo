
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

#include "mongo/platform/basic.h"

#include "mongo/scripting/mozjs/bindata.h"

#include <cctype>
#include <iomanip>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/internedstring.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"
#include "mongo/scripting/mozjs/wrapconstrainedmethod.h"
#include "mongo/util/base64.h"
#include "mongo/util/hex.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace mozjs {

const JSFunctionSpec BinDataInfo::methods[5] = {
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(base64, BinDataInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(hex, BinDataInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(toString, BinDataInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(toJSON, BinDataInfo),
    JS_FS_END,
};

const JSFunctionSpec BinDataInfo::freeFunctions[4] = {
    MONGO_ATTACH_JS_FUNCTION_WITH_FLAGS(HexData, JSFUN_CONSTRUCTOR),
    MONGO_ATTACH_JS_FUNCTION_WITH_FLAGS(MD5, JSFUN_CONSTRUCTOR),
    MONGO_ATTACH_JS_FUNCTION_WITH_FLAGS(UUID, JSFUN_CONSTRUCTOR),
    JS_FS_END,
};

const char* const BinDataInfo::className = "BinData";

namespace {

void hexToBinData(JSContext* cx,
                  int type,
                  const JS::Handle<JS::Value> hexdata,
                  JS::MutableHandleValue out) {
    auto scope = getScope(cx);
    uassert(ErrorCodes::BadValue, "BinData data must be a String", hexdata.isString());

    auto hexstr = ValueWriter(cx, hexdata).toString();

    uassert(
        ErrorCodes::BadValue, "BinData hex string must be an even length", hexstr.size() % 2 == 0);
    auto len = hexstr.size() / 2;

    std::unique_ptr<char[]> data(new char[len]);
    const char* src = hexstr.c_str();
    for (size_t i = 0; i < len; i++) {
        int src_index = i * 2;
        if (!std::isxdigit(src[src_index]) || !std::isxdigit(src[src_index + 1]))
            uasserted(ErrorCodes::BadValue, "Invalid hex character in string");
        data[i] = uassertStatusOK(fromHex(src + src_index));
    }

    std::string encoded = base64::encode(data.get(), len);
    JS::AutoValueArray<2> args(cx);

    args[0].setInt32(type);
    ValueReader(cx, args[1]).fromStringData(encoded);
    return scope->getProto<BinDataInfo>().newInstance(args, out);
}

std::string* getEncoded(JS::HandleValue thisv) {
    return static_cast<std::string*>(JS_GetPrivate(thisv.toObjectOrNull()));
}

std::string* getEncoded(JSObject* thisv) {
    return static_cast<std::string*>(JS_GetPrivate(thisv));
}

}  // namespace

void BinDataInfo::finalize(js::FreeOp* fop, JSObject* obj) {
    auto str = getEncoded(obj);

    if (str) {
        getScope(fop)->trackedDelete(str);
    }
}

void BinDataInfo::Functions::UUID::call(JSContext* cx, JS::CallArgs args) {
    boost::optional<mongo::UUID> uuid;

    if (args.length() == 0) {
        uuid = mongo::UUID::gen();
    } else {
        uassert(ErrorCodes::BadValue, "UUID needs 0 or 1 arguments", args.length() == 1);
        auto arg = args.get(0);
        std::string str = ValueWriter(cx, arg).toString();

        // For backward compatibility quietly accept and convert 32-character hex strings to
        // BinData(3, ...) as used for the deprecated UUID v3 BSON type.
        if (str.length() == 32) {
            hexToBinData(cx, bdtUUID, arg, args.rval());
            return;
        }
        uuid = uassertStatusOK(mongo::UUID::parse(str));
    };
    ConstDataRange cdr = uuid->toCDR();
    std::string encoded = mongo::base64::encode(cdr.data(), cdr.length());

    JS::AutoValueArray<2> newArgs(cx);
    newArgs[0].setInt32(newUUID);
    ValueReader(cx, newArgs[1]).fromStringData(encoded);
    getScope(cx)->getProto<BinDataInfo>().newInstance(newArgs, args.rval());
}

void BinDataInfo::Functions::MD5::call(JSContext* cx, JS::CallArgs args) {
    if (args.length() != 1)
        uasserted(ErrorCodes::BadValue, "MD5 needs 1 argument");

    auto arg = args.get(0);
    auto str = ValueWriter(cx, arg).toString();

    if (str.length() != 32)
        uasserted(ErrorCodes::BadValue, "MD5 string must have 32 characters");

    hexToBinData(cx, MD5Type, arg, args.rval());
}

void BinDataInfo::Functions::HexData::call(JSContext* cx, JS::CallArgs args) {
    if (args.length() != 2)
        uasserted(ErrorCodes::BadValue, "HexData needs 2 arguments");

    JS::RootedValue type(cx, args.get(0));

    if (!type.isNumber() || type.toInt32() < 0 || type.toInt32() > 255)
        uasserted(ErrorCodes::BadValue,
                  "HexData subtype must be a Number between 0 and 255 inclusive");

    hexToBinData(cx, type.toInt32(), args.get(1), args.rval());
}

void BinDataInfo::Functions::toString::call(JSContext* cx, JS::CallArgs args) {
    ObjectWrapper o(cx, args.thisv());

    auto str = getEncoded(args.thisv());

    str::stream ss;
    auto binType = o.getNumber(InternedString::type);

    if (binType == newUUID) {
        auto decoded = mongo::base64::decode(*str);

        // If this is in fact a UUID, use a more friendly string representation.
        if (decoded.length() == mongo::UUID::kNumBytes) {
            mongo::UUID uuid = mongo::UUID::fromCDR({decoded.data(), decoded.length()});
            ss << "UUID(\"" << uuid.toString() << "\")";
            ValueReader(cx, args.rval()).fromStringData(ss.operator std::string());
            return;
        }
    }

    ss << "BinData(" << binType << ",\"" << *str << "\")";
    ValueReader(cx, args.rval()).fromStringData(ss.operator std::string());
}

void BinDataInfo::Functions::toJSON::call(JSContext* cx, JS::CallArgs args) {
    ObjectWrapper o(cx, args.thisv());

    auto data_str = getEncoded(args.thisv());

    std::stringstream ss;
    ss << std::hex;
    ss.width(2);
    ss.fill('0');
    ss << o.getNumber(InternedString::type);

    ValueReader(cx, args.rval())
        .fromBSON(BSON("$binary" << *data_str << "$type" << ss.str()), nullptr, false);
}

void BinDataInfo::Functions::base64::call(JSContext* cx, JS::CallArgs args) {
    auto str = getEncoded(args.thisv());

    ValueReader(cx, args.rval()).fromStringData(*str);
}

void BinDataInfo::Functions::hex::call(JSContext* cx, JS::CallArgs args) {
    auto str = getEncoded(args.thisv());

    std::string data = mongo::base64::decode(*str);
    std::stringstream ss;
    ss.setf(std::ios_base::hex, std::ios_base::basefield);
    ss.fill('0');
    ss.setf(std::ios_base::right, std::ios_base::adjustfield);
    for (auto it = data.begin(); it != data.end(); ++it) {
        unsigned v = (unsigned char)*it;
        ss << std::setw(2) << v;
    }

    ValueReader(cx, args.rval()).fromStringData(ss.str());
}

void BinDataInfo::construct(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    if (args.length() != 2) {
        uasserted(ErrorCodes::BadValue, "BinData takes 2 arguments -- BinData(subtype,data)");
    }

    auto type = args.get(0);
    auto typeNumber = ValueWriter(cx, type).toInt32();
    if (!type.isNumber() || typeNumber < 0 || typeNumber > 255) {
        uasserted(ErrorCodes::BadValue,
                  "BinData subtype must be a Number between 0 and 255 inclusive");
    }

    auto utf = args.get(1);

    if (!utf.isString()) {
        uasserted(ErrorCodes::BadValue, "BinData data must be a String");
    }

    auto str = ValueWriter(cx, utf).toString();

    auto tmpBase64 = base64::decode(str);

    JS::RootedObject thisv(cx);
    scope->getProto<BinDataInfo>().newObject(&thisv);
    ObjectWrapper o(cx, thisv);

    JS::RootedValue len(cx);
    len.setInt32(tmpBase64.length());

    o.defineProperty(InternedString::len, len, JSPROP_READONLY);
    o.defineProperty(InternedString::type, type, JSPROP_READONLY);

    JS_SetPrivate(thisv, scope->trackedNew<std::string>(std::move(str)));

    args.rval().setObjectOrNull(thisv);
}

}  // namespace mozjs
}  // namespace mongo
