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

#include "mongo/scripting/mozjs/bson.h"

#include <set>

#include "mongo/scripting/mozjs/idwrapper.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"

namespace mongo {
namespace mozjs {

const char* const BSONInfo::className = "BSON";

const JSFunctionSpec BSONInfo::freeFunctions[2] = {
    MONGO_ATTACH_JS_FUNCTION(bsonWoCompare), JS_FS_END,
};

namespace {

/**
 * Holder for bson objects which tracks state for the js wrapper
 *
 * Basically, we have read only and read/write variants, and a need to manage
 * the appearance of mutable state on the read/write versions.
 */
struct BSONHolder {
    BSONHolder(const BSONObj& obj, bool ro)
        : _obj(obj.getOwned()), _resolved(false), _readOnly(ro), _altered(false) {}

    BSONObj _obj;
    bool _resolved;
    bool _readOnly;
    bool _altered;
    std::set<std::string> _removed;
};

BSONHolder* getHolder(JSObject* obj) {
    return static_cast<BSONHolder*>(JS_GetPrivate(obj));
}

}  // namespace

void BSONInfo::make(JSContext* cx, JS::MutableHandleObject obj, BSONObj bson, bool ro) {
    auto scope = getScope(cx);

    scope->getBsonProto().newInstance(obj);
    JS_SetPrivate(obj, new BSONHolder(bson, ro));
}

void BSONInfo::finalize(JSFreeOp* fop, JSObject* obj) {
    auto holder = getHolder(obj);

    if (!holder)
        return;

    delete holder;
}

void BSONInfo::enumerate(JSContext* cx, JS::HandleObject obj, JS::AutoIdVector& properties) {
    auto holder = getHolder(obj);

    if (!holder)
        return;

    BSONObjIterator i(holder->_obj);

    ObjectWrapper o(cx, obj);
    JS::RootedValue val(cx);
    JS::RootedId id(cx);

    while (i.more()) {
        BSONElement e = i.next();

        // TODO: when we get heterogenous set lookup, switch to StringData
        // rather than involving the temporary string
        if (holder->_removed.count(e.fieldName()))
            continue;

        ValueReader(cx, &val).fromStringData(e.fieldNameStringData());

        if (!JS_ValueToId(cx, val, &id))
            uasserted(ErrorCodes::JSInterpreterFailure, "Failed to invoke JS_ValueToId");

        properties.append(id);
    }
}

void BSONInfo::setProperty(
    JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool strict, JS::MutableHandleValue vp) {
    auto holder = getHolder(obj);

    if (holder) {
        if (holder->_readOnly) {
            uasserted(ErrorCodes::BadValue, "Read only object");
        }

        auto iter = holder->_removed.find(IdWrapper(cx, id).toString());

        if (iter != holder->_removed.end()) {
            holder->_removed.erase(iter);
        }

        holder->_altered = true;
    }

    ObjectWrapper(cx, obj).defineProperty(id, vp, JSPROP_ENUMERATE);
}

void BSONInfo::delProperty(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* succeeded) {
    auto holder = getHolder(obj);

    if (holder) {
        if (holder->_readOnly) {
            uasserted(ErrorCodes::BadValue, "Read only object");
        }

        holder->_altered = true;

        holder->_removed.insert(IdWrapper(cx, id).toString());
    }

    *succeeded = true;
}

void BSONInfo::resolve(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* resolvedp) {
    auto holder = getHolder(obj);

    *resolvedp = false;

    if (!holder) {
        return;
    }

    IdWrapper idw(cx, id);

    if (!holder->_readOnly && holder->_removed.count(idw.toString())) {
        return;
    }

    ObjectWrapper o(cx, obj);

    std::string sname = IdWrapper(cx, id).toString();

    if (holder->_obj.hasField(sname)) {
        auto elem = holder->_obj[sname];

        JS::RootedValue vp(cx);

        ValueReader(cx, &vp).fromBSONElement(elem, holder->_readOnly);

        o.defineProperty(id, vp, JSPROP_ENUMERATE);

        if (!holder->_readOnly && (elem.type() == mongo::Object || elem.type() == mongo::Array)) {
            // if accessing a subobject, we have no way to know if
            // modifications are being made on writable objects

            holder->_altered = true;
        }

        *resolvedp = true;
    }
}

void BSONInfo::construct(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    scope->getBsonProto().newObject(args.rval());
}

std::tuple<BSONObj*, bool> BSONInfo::originalBSON(JSContext* cx, JS::HandleObject obj) {
    std::tuple<BSONObj*, bool> out(nullptr, false);

    if (auto holder = getHolder(obj))
        out = std::make_tuple(&holder->_obj, holder->_altered);

    return out;
}

void BSONInfo::Functions::bsonWoCompare(JSContext* cx, JS::CallArgs args) {
    if (args.length() != 2)
        uasserted(ErrorCodes::BadValue, "bsonWoCompare needs 2 argument");

    if (!args.get(0).isObject())
        uasserted(ErrorCodes::BadValue, "first argument to bsonWoCompare must be an object");

    if (!args.get(1).isObject())
        uasserted(ErrorCodes::BadValue, "second argument to bsonWoCompare must be an object");

    BSONObj firstObject = ValueWriter(cx, args.get(0)).toBSON();
    BSONObj secondObject = ValueWriter(cx, args.get(1)).toBSON();

    args.rval().setInt32(firstObject.woCompare(secondObject));
}

void BSONInfo::postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto) {
    JS::RootedValue value(cx);
    value.setBoolean(true);

    ObjectWrapper(cx, proto).defineProperty("_bson", value, 0);
}

}  // namespace mozjs
}  // namespace mongo
