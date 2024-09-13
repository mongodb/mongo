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
#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <fmt/format.h>
#include <js/CallArgs.h>
#include <js/Object.h>
#include <js/PropertyDescriptor.h>
#include <js/RootingAPI.h>
#include <jsapi.h>
#include <jscustomallocator.h>

#include <js/Class.h>
#include <js/PropertySpec.h>
#include <js/TypeDecls.h>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_comparator_interface_base.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/scripting/mozjs/bson.h"
#include "mongo/scripting/mozjs/idwrapper.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/internedstring.h"
#include "mongo/scripting/mozjs/jsstringwrapper.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

namespace mongo {
namespace mozjs {

using namespace fmt::literals;

const char* const BSONInfo::className = "BSON";

const JSFunctionSpec BSONInfo::freeFunctions[5] = {
    MONGO_ATTACH_JS_FUNCTION(bsonWoCompare),
    MONGO_ATTACH_JS_FUNCTION(bsonUnorderedFieldsCompare),
    MONGO_ATTACH_JS_FUNCTION(bsonBinaryEqual),
    MONGO_ATTACH_JS_FUNCTION(bsonObjToArray),
    JS_FS_END,
};


namespace {

BSONObj getBSONFromArg(JSContext* cx, JS::HandleValue arg, bool isBSON) {
    if (isBSON) {
        return ValueWriter(cx, arg).toBSON();
    }
    JS::RootedObject rout(cx, JS_NewPlainObject(cx));
    ObjectWrapper object(cx, rout);
    object.setValue("a", arg);
    return object.toBSON();
}

/**
 * Holder for bson objects which tracks state for the js wrapper
 *
 * Basically, we have read only and read/write variants, and a need to manage
 * the appearance of mutable state on the read/write versions.
 */
struct BSONHolder {
    BSONHolder(const BSONObj& obj, const BSONObj* parent, const MozJSImplScope* scope, bool ro)
        : _obj(obj),
          _generation(scope->getGeneration()),
          _isOwned(obj.isOwned() || (parent && parent->isOwned())),
          _resolved(false),
          _readOnly(ro),
          _altered(false) {
        uassert(
            ErrorCodes::BadValue,
            "Attempt to bind an unowned BSON Object to a JS scope marked as requiring ownership",
            _isOwned || (!scope->requiresOwnedObjects()));
        if (parent) {
            _parent.emplace(*parent);
        }
    }

    const BSONObj& getOwner() const {
        return _parent ? *(_parent) : _obj;
    }

    void uassertValid(JSContext* cx) const {
        if (!_isOwned && getScope(cx)->getGeneration() != _generation)
            uasserted(ErrorCodes::BadValue,
                      "Attempt to access an invalidated BSON Object in JS scope");
    }

    BSONObj _obj;
    boost::optional<BSONObj> _parent;
    std::size_t _generation;
    bool _isOwned;
    bool _resolved;
    bool _readOnly;
    bool _altered;
    StringMap<bool> _removed;
};

BSONHolder* getValidHolder(JSContext* cx, JSObject* obj) {
    auto holder = JS::GetMaybePtrFromReservedSlot<BSONHolder>(obj, BSONInfo::BSONHolderSlot);

    if (holder)
        holder->uassertValid(cx);

    return holder;
}

void definePropertyFromBSONElement(JSContext* cx,
                                   BSONHolder& holder,
                                   const BSONElement& elem,
                                   JS::HandleObject obj,
                                   JS::HandleId id) {
    JS::RootedValue vp(cx);
    ValueReader(cx, &vp).fromBSONElement(elem, holder.getOwner(), holder._readOnly);
    ObjectWrapper o(cx, obj);
    o.defineProperty(id, vp, JSPROP_ENUMERATE);

    if (!holder._readOnly && (elem.type() == mongo::Object || elem.type() == mongo::Array)) {
        // if accessing a subobject, we have no way to know if
        // modifications are being made on writable objects

        holder._altered = true;
    }
}
}  // namespace

void BSONInfo::make(
    JSContext* cx, JS::MutableHandleObject obj, BSONObj bson, const BSONObj* parent, bool ro) {
    auto scope = getScope(cx);

    scope->getProto<BSONInfo>().newObject(obj);
    JS::SetReservedSlot(obj,
                        BSONHolderSlot,
                        JS::PrivateValue(scope->trackedNew<BSONHolder>(bson, parent, scope, ro)));
}

void BSONInfo::finalize(JS::GCContext* gcCtx, JSObject* obj) {
    auto holder = JS::GetMaybePtrFromReservedSlot<BSONHolder>(obj, BSONHolderSlot);

    if (!holder)
        return;

    getScope(gcCtx)->trackedDelete(holder);
}

/*
 * BSONInfo::enumerate() implements the "NewEnumerate" operation (i.e JSNewEnumerateOp). This method
 * enumerates the keys, and resolves the lazy property values by defining them on the JSObject.
 * Historically, the new enumerate did not resolve lazy property values, and as a result, did not
 * service calls to Object.entries() correctly. Note, when a property value is defined (see
 * definePropertyFromBSONElement() below), the property is defined in the underlying NativeObject.
 * Although this is closer to the intended behaviour of the old enumerate (i.e JsEnumerateOp), we
 * must still implement this as a "NewEnumerate" hook. During enumeration, if a JsNewEnumerateOp is
 * not provided, the properties are enumerated in the order in which they appear in the
 * NativeObject, which is not suitable for our use case. Consider a JSObject wrapping a
 * BSONObj{a:"a", b:"b"}. If a new property "c" is added to the JSObject, the properties will appear
 * in the order ["c","a","b"] instead of ["a","b","c"] during enumeration. The same issue occurs
 * if a property of the BSONObj is modified in the JSObject (modified properties appear first in the
 * enumeration). In contrast, when a JsNewEnumerateOp hook is provided, "extra" properties are
 * enumerated first, followed by "native" properties. These "extra" properties correspond to the
 * properties enumerated via the new enumerate hook below. In short, by enumerating the BSONObj as
 * "extra" properties in the "NewEnumerate" hook, we guarantee the properties in the original
 * BSONObj appear first during enumeration, irrespective of any updates to the JSObject.
 */
void BSONInfo::enumerate(JSContext* cx,
                         JS::HandleObject obj,
                         JS::MutableHandleIdVector properties,
                         bool enumerableOnly) {
    auto holder = getValidHolder(cx, obj);

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
        auto fieldNameStringData = e.fieldNameStringData();
        if (holder->_removed.find(fieldNameStringData.toString()) != holder->_removed.end())
            continue;

        ValueReader(cx, &val).fromStringData(fieldNameStringData);

        if (!JS_ValueToId(cx, val, &id))
            uasserted(ErrorCodes::JSInterpreterFailure, "Failed to invoke JS_ValueToId");

        bool isAlreadyDefined{false};
        if (!JS_AlreadyHasOwnPropertyById(cx, obj, id, &isAlreadyDefined)) {
            uasserted(ErrorCodes::JSInterpreterFailure,
                      "Failed to invoke JS_AlreadyHasOwnPropertyById");
        }

        // Only define a property during enumeration if it hasn't already been defined.
        if (!isAlreadyDefined) {
            definePropertyFromBSONElement(cx, *holder, e, obj, id);
        }

        if (!properties.append(id))
            uasserted(ErrorCodes::JSInterpreterFailure, "Failed to append property");
    }
}

void BSONInfo::setProperty(JSContext* cx,

                           JS::HandleObject obj,
                           JS::HandleId id,
                           JS::HandleValue vp,
                           JS::HandleValue receiver,
                           JS::ObjectOpResult& result) {

    auto holder = getValidHolder(cx, obj);

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
    result.succeed();
}

void BSONInfo::delProperty(JSContext* cx,
                           JS::HandleObject obj,
                           JS::HandleId id,
                           JS::ObjectOpResult& result) {
    auto holder = getValidHolder(cx, obj);

    if (holder) {
        if (holder->_readOnly) {
            uasserted(ErrorCodes::BadValue, "Read only object");
            return;
        }

        holder->_altered = true;

        JSStringWrapper jsstr;
        holder->_removed[IdWrapper(cx, id).toStringData(&jsstr)] = true;
    }

    result.succeed();
}

void BSONInfo::resolve(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* resolvedp) {
    auto holder = getValidHolder(cx, obj);

    *resolvedp = false;

    if (!holder) {
        return;
    }

    IdWrapper idw(cx, id);
    JSStringWrapper jsstr;

    auto sname = idw.toStringData(&jsstr);
    if (sname.find('\0') != std::string::npos)
        return;
    if (!holder->_readOnly && holder->_removed.find(sname.toString()) != holder->_removed.end())
        return;
    if (!holder->_obj.hasField(sname))
        return;
    definePropertyFromBSONElement(cx, *holder, holder->_obj[sname], obj, id);
    *resolvedp = true;
    return;
}

std::tuple<BSONObj*, bool> BSONInfo::originalBSON(JSContext* cx, JS::HandleObject obj) {
    std::tuple<BSONObj*, bool> out(nullptr, false);

    if (auto holder = getValidHolder(cx, obj))
        out = std::make_tuple(&holder->_obj, holder->_altered);

    return out;
}

void BSONInfo::Functions::bsonObjToArray::call(JSContext* cx, JS::CallArgs args) {
    uassert(ErrorCodes::BadValue, "bsonObjToArray needs 1 argument", args.length() == 1);
    uassert(ErrorCodes::BadValue, "argument must be an object", args.get(0).isObject());

    auto obj = ValueWriter(cx, args.get(0)).toBSON();
    ValueReader(cx, args.rval()).fromBSONArray(obj, nullptr, false);
}

namespace {
void bsonCompareCommon(JSContext* cx,
                       JS::CallArgs args,
                       StringData funcName,
                       BSONObj::ComparisonRulesSet rules) {
    if (args.length() != 2)
        uasserted(ErrorCodes::BadValue, "{} needs 2 arguments"_format(funcName));

    // If either argument is not proper BSON, then we wrap both objects.
    auto scope = getScope(cx);
    bool isBSON = scope->getProto<BSONInfo>().instanceOf(args.get(0)) &&
        scope->getProto<BSONInfo>().instanceOf(args.get(1));

    BSONObj bsonObject1 = getBSONFromArg(cx, args.get(0), isBSON);
    BSONObj bsonObject2 = getBSONFromArg(cx, args.get(1), isBSON);

    args.rval().setInt32(bsonObject1.woCompare(bsonObject2, {}, rules));
}
}  // namespace

void BSONInfo::Functions::bsonWoCompare::call(JSContext* cx, JS::CallArgs args) {
    bsonCompareCommon(cx, args, "bsonWoCompare", BSONObj::ComparatorInterface::kConsiderFieldName);
}

void BSONInfo::Functions::bsonUnorderedFieldsCompare::call(JSContext* cx, JS::CallArgs args) {
    bsonCompareCommon(cx,
                      args,
                      "bsonWoCompare",
                      BSONObj::ComparatorInterface::kConsiderFieldName |
                          BSONObj::ComparatorInterface::kIgnoreFieldOrder);
}

void BSONInfo::Functions::bsonBinaryEqual::call(JSContext* cx, JS::CallArgs args) {
    if (args.length() != 2)
        uasserted(ErrorCodes::BadValue, "bsonBinaryEqual needs 2 arguments");

    // If either argument is not a proper BSON, then we wrap both objects.
    auto scope = getScope(cx);
    bool isBSON = scope->getProto<BSONInfo>().instanceOf(args.get(0)) &&
        scope->getProto<BSONInfo>().instanceOf(args.get(1));

    BSONObj bsonObject1 = getBSONFromArg(cx, args.get(0), isBSON);
    BSONObj bsonObject2 = getBSONFromArg(cx, args.get(1), isBSON);

    args.rval().setBoolean(bsonObject1.binaryEqual(bsonObject2));
}

void BSONInfo::postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto) {
    JS::RootedValue value(cx);
    value.setBoolean(true);

    ObjectWrapper(cx, proto).defineProperty(InternedString::_bson, value, 0);
}

}  // namespace mozjs
}  // namespace mongo
