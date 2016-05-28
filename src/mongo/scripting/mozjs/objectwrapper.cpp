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

#include "mongo/scripting/mozjs/objectwrapper.h"

#include <js/Conversions.h>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/decimal128.h"
#include "mongo/scripting/mozjs/idwrapper.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"

namespace mongo {
namespace mozjs {

const int ObjectWrapper::kMaxWriteFieldDepth;

void ObjectWrapper::Key::get(JSContext* cx, JS::HandleObject o, JS::MutableHandleValue value) {
    switch (_type) {
        case Type::Field:
            if (JS_GetProperty(cx, o, _field, value))
                return;
            break;
        case Type::Index:
            if (JS_GetElement(cx, o, _idx, value))
                return;
            break;
        case Type::Id: {
            JS::RootedId id(cx, _id);

            if (JS_GetPropertyById(cx, o, id, value))
                return;
            break;
        }
        case Type::InternedString: {
            InternedStringId id(cx, _internedString);

            if (JS_GetPropertyById(cx, o, id, value))
                return;
            break;
        }
    }

    throwCurrentJSException(cx, ErrorCodes::InternalError, "Failed to get value on a JSObject");
}

void ObjectWrapper::Key::set(JSContext* cx, JS::HandleObject o, JS::HandleValue value) {
    switch (_type) {
        case Type::Field:
            if (JS_SetProperty(cx, o, _field, value))
                return;
            break;
        case Type::Index:
            if (JS_SetElement(cx, o, _idx, value))
                return;
            break;
        case Type::Id: {
            JS::RootedId id(cx, _id);

            if (JS_SetPropertyById(cx, o, id, value))
                return;
            break;
        }
        case Type::InternedString: {
            InternedStringId id(cx, _internedString);

            if (JS_SetPropertyById(cx, o, id, value))
                return;
            break;
        }
    }

    throwCurrentJSException(cx, ErrorCodes::InternalError, "Failed to set value on a JSObject");
}

void ObjectWrapper::Key::define(JSContext* cx,
                                JS::HandleObject o,
                                JS::HandleValue value,
                                unsigned attrs) {
    switch (_type) {
        case Type::Field:
            if (JS_DefineProperty(cx, o, _field, value, attrs))
                return;
            break;
        case Type::Index:
            if (JS_DefineElement(cx, o, _idx, value, attrs))
                return;
            break;
        case Type::Id: {
            JS::RootedId id(cx, _id);

            if (JS_DefinePropertyById(cx, o, id, value, attrs))
                return;
            break;
        }
        case Type::InternedString: {
            InternedStringId id(cx, _internedString);

            if (JS_DefinePropertyById(cx, o, id, value, attrs))
                return;
            break;
        }
    }

    throwCurrentJSException(cx, ErrorCodes::InternalError, "Failed to define value on a JSObject");
}

bool ObjectWrapper::Key::has(JSContext* cx, JS::HandleObject o) {
    bool has;

    switch (_type) {
        case Type::Field:
            if (JS_HasProperty(cx, o, _field, &has))
                return has;
            break;
        case Type::Index:
            if (JS_HasElement(cx, o, _idx, &has))
                return has;
            break;
        case Type::Id: {
            JS::RootedId id(cx, _id);

            if (JS_HasPropertyById(cx, o, id, &has))
                return has;
            break;
        }
        case Type::InternedString: {
            InternedStringId id(cx, _internedString);

            if (JS_HasPropertyById(cx, o, id, &has))
                return has;
            break;
        }
    }

    throwCurrentJSException(cx, ErrorCodes::InternalError, "Failed to has value on a JSObject");
}

bool ObjectWrapper::Key::hasOwn(JSContext* cx, JS::HandleObject o) {
    bool has;

    switch (_type) {
        case Type::Field:
            if (JS_AlreadyHasOwnProperty(cx, o, _field, &has))
                return has;
            break;
        case Type::Index:
            if (JS_AlreadyHasOwnElement(cx, o, _idx, &has))
                return has;
            break;
        case Type::Id: {
            JS::RootedId id(cx, _id);

            if (JS_AlreadyHasOwnPropertyById(cx, o, id, &has))
                return has;
            break;
        }
        case Type::InternedString: {
            InternedStringId id(cx, _internedString);

            if (JS_AlreadyHasOwnPropertyById(cx, o, id, &has))
                return has;
            break;
        }
    }

    throwCurrentJSException(cx, ErrorCodes::InternalError, "Failed to hasOwn value on a JSObject");
}

void ObjectWrapper::Key::del(JSContext* cx, JS::HandleObject o) {
    switch (_type) {
        case Type::Field:
            if (JS_DeleteProperty(cx, o, _field))
                return;
            break;
        case Type::Index:
            if (JS_DeleteElement(cx, o, _idx))
                return;
            break;
        case Type::Id: {
            JS::RootedId id(cx, _id);

            // For some reason JS_DeletePropertyById doesn't link
            if (JS_DeleteProperty(cx, o, IdWrapper(cx, id).toString().c_str()))
                return;
            break;
        }
        case Type::InternedString: {
            InternedStringId id(cx, _internedString);

            if (JS_DeleteProperty(cx, o, IdWrapper(cx, id).toString().c_str()))
                break;
        }
    }

    throwCurrentJSException(cx, ErrorCodes::InternalError, "Failed to delete value on a JSObject");
}

std::string ObjectWrapper::Key::toString(JSContext* cx) {
    JSStringWrapper jsstr;
    return toStringData(cx, &jsstr).toString();
}

StringData ObjectWrapper::Key::toStringData(JSContext* cx, JSStringWrapper* jsstr) {
    if (_type == Type::Field) {
        return _field;
    }

    if (_type == Type::Index) {
        *jsstr = JSStringWrapper(_idx);
        return jsstr->toStringData();
    }

    JS::RootedId rid(cx);

    if (_type == Type::Id) {
        rid.set(_id);
    } else {
        InternedStringId id(cx, _internedString);
        rid.set(id);
    }

    if (JSID_IS_INT(rid)) {
        *jsstr = JSStringWrapper(JSID_TO_INT(rid));
        return jsstr->toStringData();
    }

    if (JSID_IS_STRING(rid)) {
        *jsstr = JSStringWrapper(cx, JSID_TO_STRING(rid));
        return jsstr->toStringData();
    }

    uasserted(ErrorCodes::BadValue, "Couldn't convert key to String");
}

ObjectWrapper::ObjectWrapper(JSContext* cx, JS::HandleObject obj)
    : _context(cx), _object(cx, obj) {}

ObjectWrapper::ObjectWrapper(JSContext* cx, JS::HandleValue value)
    : _context(cx), _object(cx, value.toObjectOrNull()) {}

double ObjectWrapper::getNumber(Key key) {
    JS::RootedValue x(_context);
    getValue(key, &x);

    return ValueWriter(_context, x).toNumber();
}

int ObjectWrapper::getNumberInt(Key key) {
    JS::RootedValue x(_context);
    getValue(key, &x);

    return ValueWriter(_context, x).toInt32();
}

long long ObjectWrapper::getNumberLongLong(Key key) {
    JS::RootedValue x(_context);
    getValue(key, &x);

    return ValueWriter(_context, x).toInt64();
}

Decimal128 ObjectWrapper::getNumberDecimal(Key key) {
    JS::RootedValue x(_context);
    getValue(key, &x);

    return ValueWriter(_context, x).toDecimal128();
}

std::string ObjectWrapper::getString(Key key) {
    JS::RootedValue x(_context);
    getValue(key, &x);

    return ValueWriter(_context, x).toString();
}

bool ObjectWrapper::getBoolean(Key key) {
    JS::RootedValue x(_context);
    getValue(key, &x);

    return ValueWriter(_context, x).toBoolean();
}

BSONObj ObjectWrapper::getObject(Key key) {
    JS::RootedValue x(_context);
    getValue(key, &x);

    return ValueWriter(_context, x).toBSON();
}

void ObjectWrapper::getValue(Key key, JS::MutableHandleValue value) {
    key.get(_context, _object, value);
}

void ObjectWrapper::setNumber(Key key, double val) {
    JS::RootedValue jsValue(_context);
    ValueReader(_context, &jsValue).fromDouble(val);

    setValue(key, jsValue);
}

void ObjectWrapper::setString(Key key, StringData val) {
    JS::RootedValue jsValue(_context);
    ValueReader(_context, &jsValue).fromStringData(val);

    setValue(key, jsValue);
}

void ObjectWrapper::setBoolean(Key key, bool val) {
    JS::RootedValue jsValue(_context);
    jsValue.setBoolean(val);

    setValue(key, jsValue);
}

void ObjectWrapper::setBSONElement(Key key,
                                   const BSONElement& elem,
                                   const BSONObj& parent,
                                   bool readOnly) {
    JS::RootedValue value(_context);
    ValueReader(_context, &value).fromBSONElement(elem, parent, readOnly);

    setValue(key, value);
}

void ObjectWrapper::setBSON(Key key, const BSONObj& obj, bool readOnly) {
    JS::RootedValue value(_context);
    ValueReader(_context, &value).fromBSON(obj, nullptr, readOnly);

    setValue(key, value);
}

void ObjectWrapper::setValue(Key key, JS::HandleValue val) {
    key.set(_context, _object, val);
}

void ObjectWrapper::setObject(Key key, JS::HandleObject object) {
    JS::RootedValue value(_context);
    value.setObjectOrNull(object);

    setValue(key, value);
}

void ObjectWrapper::defineProperty(Key key, JS::HandleValue val, unsigned attrs) {
    key.define(_context, _object, val, attrs);
}

void ObjectWrapper::deleteProperty(Key key) {
    key.del(_context, _object);
}

int ObjectWrapper::type(Key key) {
    JS::RootedValue x(_context);
    getValue(key, &x);

    return ValueWriter(_context, x).type();
}

void ObjectWrapper::rename(Key from, const char* to) {
    JS::RootedValue value(_context);

    JS::RootedValue undefValue(_context);
    undefValue.setUndefined();

    getValue(from, &value);

    setValue(to, value);
    setValue(from, undefValue);
}

bool ObjectWrapper::hasField(Key key) {
    return key.has(_context, _object);
}

bool ObjectWrapper::hasOwnField(Key key) {
    return key.hasOwn(_context, _object);
}

void ObjectWrapper::callMethod(const char* field,
                               const JS::HandleValueArray& args,
                               JS::MutableHandleValue out) {
    if (JS::Call(_context, _object, field, args, out))
        return;

    throwCurrentJSException(_context, ErrorCodes::InternalError, "Failed to call method");
}

void ObjectWrapper::callMethod(const char* field, JS::MutableHandleValue out) {
    JS::AutoValueVector args(_context);

    callMethod(field, args, out);
}

void ObjectWrapper::callMethod(JS::HandleValue fun,
                               const JS::HandleValueArray& args,
                               JS::MutableHandleValue out) {
    if (JS::Call(_context, _object, fun, args, out))
        return;

    throwCurrentJSException(_context, ErrorCodes::InternalError, "Failed to call method");
}

void ObjectWrapper::callMethod(JS::HandleValue fun, JS::MutableHandleValue out) {
    JS::AutoValueVector args(_context);

    callMethod(fun, args, out);
}

BSONObj ObjectWrapper::toBSON() {
    if (getScope(_context)->getProto<BSONInfo>().instanceOf(_object)) {
        BSONObj* originalBSON = nullptr;
        bool altered;

        std::tie(originalBSON, altered) = BSONInfo::originalBSON(_context, _object);

        if (originalBSON && !altered)
            return *originalBSON;
    }

    JS::RootedId id(_context);

    // INCREDIBLY SUBTLE BEHAVIOR:
    //
    // (jcarey): Be very careful about how the Rooting API is used in
    // relationship to WriteFieldRecursionFrames. Mozilla'a API more or less
    // demands that the rooting types are on the stack and only manipulated as
    // regular objects, which we aren't doing here. The reason they do this is
    // because the rooting types must be global created and destroyed in an
    // entirely linear order. This is impossible to screw up in regular use,
    // but our unwinding of the recursion frames makes it easy to do here.
    //
    // The roots above need to be before the first frame is emplaced (so
    // they'll be destroyed after it) and none of the roots in the below code
    // (or in ValueWriter::writeThis) can live longer than until the call to
    // emplace() inside ValueWriter. The runtime asserts enabled by MozJS's
    // debug mode will catch runtime errors, but be aware of how difficult this
    // is to get right and what to look for if one of them bites you.

    BSONObjBuilder b;

    {
        // NOTE: Keep the frames in a scope so that it is clear that
        // we always destroy them before we destroy 'b'. It is
        // important to do so: if 'b' is destroyed before the frames,
        // and we don't pop all of the frames (say, due to an
        // exeption), then the frame dtors would write to freed
        // memory.
        WriteFieldRecursionFrames frames;
        frames.emplace(_context, _object, nullptr, StringData{});

        // We special case the _id field in top-level objects and move it to the front.
        // This matches other drivers behavior and makes finding the _id field quicker in BSON.
        if (hasOwnField(InternedString::_id)) {
            _writeField(&b, InternedString::_id, &frames, frames.top().originalBSON);
        }

        while (frames.size()) {
            auto& frame = frames.top();

            // If the index is the same as length, we've seen all the keys at this
            // level and should go up a level
            if (frame.idx == frame.ids.length()) {
                frames.pop();
                continue;
            }

            if (frame.idx == 0 && frame.originalBSON && !frame.altered) {
                // If this is our first look at the object and it has an unaltered
                // bson behind it, move idx to the end so we'll roll up on the next
                // pass through the loop.
                frame.subbob_or(&b)->appendElements(*frame.originalBSON);
                frame.idx = frame.ids.length();
                continue;
            }

            id.set(frame.ids[frame.idx++]);

            if (frames.size() == 1) {
                IdWrapper idw(_context, id);

                // TODO: check if it's cheaper to just compare with an interned
                // string of "_id" rather than with ascii
                if (idw.isString() && idw.equalsAscii("_id")) {
                    continue;
                }
            }

            // writeField invokes ValueWriter with the frame stack, which will push
            // onto frames for subobjects, which will effectively recurse the loop.
            _writeField(frame.subbob_or(&b), JS::HandleId(id), &frames, frame.originalBSON);
        }
    }

    const int sizeWithEOO = b.len() + 1 /*EOO*/ - 4 /*BSONObj::Holder ref count*/;
    uassert(17260,
            str::stream() << "Converting from JavaScript to BSON failed: "
                          << "Object size "
                          << sizeWithEOO
                          << " exceeds limit of "
                          << BSONObjMaxInternalSize
                          << " bytes.",
            sizeWithEOO <= BSONObjMaxInternalSize);

    return b.obj();
}

ObjectWrapper::WriteFieldRecursionFrame::WriteFieldRecursionFrame(JSContext* cx,
                                                                  JSObject* obj,
                                                                  BSONObjBuilder* parent,
                                                                  StringData sd)
    : thisv(cx, obj), ids(cx, JS::IdVector(cx)) {
    if (parent) {
        bool isArray;

        if (!JS_IsArrayObject(cx, thisv, &isArray)) {
            throwCurrentJSException(
                cx, ErrorCodes::JSInterpreterFailure, "Failure to check object is an array");
        }

        subbob.emplace(isArray ? parent->subarrayStart(sd) : parent->subobjStart(sd));
    }

    if (!JS_Enumerate(cx, thisv, &ids)) {
        throwCurrentJSException(
            cx, ErrorCodes::JSInterpreterFailure, "Failure to enumerate object");
    }

    if (getScope(cx)->getProto<BSONInfo>().instanceOf(thisv)) {
        std::tie(originalBSON, altered) = BSONInfo::originalBSON(cx, thisv);
    }
}

void ObjectWrapper::_writeField(BSONObjBuilder* b,
                                Key key,
                                WriteFieldRecursionFrames* frames,
                                BSONObj* originalParent) {
    JS::RootedValue value(_context);
    key.get(_context, frames->top().thisv, &value);

    ValueWriter x(_context, value);
    x.setOriginalBSON(originalParent);

    JSStringWrapper jsstr;

    x.writeThis(b, key.toStringData(_context, &jsstr), frames);
}

std::string ObjectWrapper::getClassName() {
    auto jsclass = JS_GetClass(_object);

    if (jsclass)
        return jsclass->name;

    JS::RootedValue ctor(_context);
    getValue(InternedString::constructor, &ctor);

    return ObjectWrapper(_context, ctor).getString(InternedString::name);
}

}  // namespace mozjs
}  // namespace mongo
