/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/scripting/mozjs/bindata.h"

#include <iomanip>

#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"
#include "mongo/util/base64.h"
#include "mongo/util/hex.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace mozjs {

const JSFunctionSpec BinDataInfo::methods[4] = {
    MONGO_ATTACH_JS_FUNCTION(base64),
    MONGO_ATTACH_JS_FUNCTION(hex),
    MONGO_ATTACH_JS_FUNCTION(toString),
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

void hexToBinData(JSContext* cx, int type, StringData hexstr, JS::MutableHandleValue out) {
    auto scope = getScope(cx);

    // SERVER-9686: This function does not correctly check to make sure hexstr is actually made
    // up of valid hex digits, and fails in the hex utility functions

    int len = hexstr.size() / 2;
    std::unique_ptr<char[]> data(new char[len]);
    const char* src = hexstr.rawData();
    for (int i = 0; i < len; i++) {
        data[i] = fromHex(src + i * 2);
    }

    std::string encoded = base64::encode(data.get(), len);
    JS::AutoValueArray<2> args(cx);

    args[0].setInt32(type);
    ValueReader(cx, args[1]).fromStringData(encoded);
    return scope->getBinDataProto().newInstance(args, out);
}

std::string* getEncoded(JS::HandleValue thisv) {
    return static_cast<std::string*>(JS_GetPrivate(thisv.toObjectOrNull()));
}

std::string* getEncoded(JSObject* thisv) {
    return static_cast<std::string*>(JS_GetPrivate(thisv));
}

}  // namespace

void BinDataInfo::finalize(JSFreeOp* fop, JSObject* obj) {
    auto str = getEncoded(obj);

    if (str) {
        delete str;
    }
}

void BinDataInfo::Functions::UUID(JSContext* cx, JS::CallArgs args) {
    if (args.length() != 1)
        uasserted(ErrorCodes::BadValue, "UUID needs 1 argument");

    auto str = ValueWriter(cx, args.get(0)).toString();

    if (str.length() != 32)
        uasserted(ErrorCodes::BadValue, "UUID string must have 32 characters");

    hexToBinData(cx, bdtUUID, str, args.rval());
}

void BinDataInfo::Functions::MD5(JSContext* cx, JS::CallArgs args) {
    if (args.length() != 1)
        uasserted(ErrorCodes::BadValue, "MD5 needs 1 argument");

    auto str = ValueWriter(cx, args.get(0)).toString();

    if (str.length() != 32)
        uasserted(ErrorCodes::BadValue, "MD5 string must have 32 characters");

    hexToBinData(cx, MD5Type, str, args.rval());
}

void BinDataInfo::Functions::HexData(JSContext* cx, JS::CallArgs args) {
    if (args.length() != 2)
        uasserted(ErrorCodes::BadValue, "HexData needs 2 arguments");

    JS::RootedValue type(cx, args.get(0));

    if (!type.isNumber() || type.toInt32() < 0 || type.toInt32() > 255)
        uasserted(ErrorCodes::BadValue,
                  "HexData subtype must be a Number between 0 and 255 inclusive");

    auto str = ValueWriter(cx, args.get(1)).toString();

    hexToBinData(cx, type.toInt32(), str, args.rval());
}

void BinDataInfo::Functions::toString(JSContext* cx, JS::CallArgs args) {
    ObjectWrapper o(cx, args.thisv());

    auto str = getEncoded(args.thisv());

    str::stream ss;

    ss << "BinData(" << o.getNumber("type") << ",\"" << *str << "\")";

    ValueReader(cx, args.rval()).fromStringData(ss.operator std::string());
}

void BinDataInfo::Functions::base64(JSContext* cx, JS::CallArgs args) {
    auto str = getEncoded(args.thisv());

    ValueReader(cx, args.rval()).fromStringData(*str);
}

void BinDataInfo::Functions::hex(JSContext* cx, JS::CallArgs args) {
    auto str = getEncoded(args.thisv());

    std::string data = base64::decode(*str);
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

    if (!type.isNumber() || type.toInt32() < 0 || type.toInt32() > 255) {
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
    scope->getBinDataProto().newObject(&thisv);
    ObjectWrapper o(cx, thisv);

    JS::RootedValue len(cx);
    len.setInt32(tmpBase64.length());

    o.defineProperty("len", len, JSPROP_READONLY);
    o.defineProperty("type", type, JSPROP_READONLY);

    JS_SetPrivate(thisv, new std::string(std::move(str)));

    args.rval().setObjectOrNull(thisv);
}

}  // namespace mozjs
}  // namespace mongo
