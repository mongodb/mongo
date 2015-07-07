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

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/decimal128.h"
#include "mongo/scripting/mozjs/idwrapper.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"

namespace mongo {
namespace mozjs {

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
    }

    throwCurrentJSException(cx, ErrorCodes::InternalError, "Failed to has value on a JSObject");
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
    }

    throwCurrentJSException(cx, ErrorCodes::InternalError, "Failed to delete value on a JSObject");
}

std::string ObjectWrapper::Key::toString(JSContext* cx) {
    switch (_type) {
        case Type::Field:
            return _field;
        case Type::Index:
            return std::to_string(_idx);
        case Type::Id: {
            JS::RootedId id(cx, _id);
            return IdWrapper(cx, id).toString();
        }
    }

    throwCurrentJSException(
        cx, ErrorCodes::InternalError, "Failed to toString a ObjectWrapper::Key");
}

ObjectWrapper::ObjectWrapper(JSContext* cx, JS::HandleObject obj, int depth)
    : _context(cx), _object(cx, obj), _depth(depth) {}

ObjectWrapper::ObjectWrapper(JSContext* cx, JS::HandleValue value, int depth)
    : _context(cx), _object(cx, value.toObjectOrNull()), _depth(depth) {}

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

    return ValueWriter(_context, x, _depth).toBSON();
}

void ObjectWrapper::getValue(Key key, JS::MutableHandleValue value) {
    key.get(_context, _object, value);
}

void ObjectWrapper::setNumber(Key key, double val) {
    JS::RootedValue jsValue(_context);
    jsValue.setDouble(val);

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

void ObjectWrapper::setBSONElement(Key key, const BSONElement& elem, bool readOnly) {
    JS::RootedValue value(_context);
    ValueReader(_context, &value, _depth).fromBSONElement(elem, readOnly);

    setValue(key, value);
}

void ObjectWrapper::setBSON(Key key, const BSONObj& obj, bool readOnly) {
    JS::RootedValue value(_context);
    ValueReader(_context, &value, _depth).fromBSON(obj, readOnly);

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

void ObjectWrapper::writeThis(BSONObjBuilder* b) {
    auto scope = getScope(_context);

    BSONObj* originalBSON = nullptr;
    if (scope->getBsonProto().instanceOf(_object)) {
        bool altered;

        std::tie(originalBSON, altered) = BSONInfo::originalBSON(_context, _object);

        if (originalBSON && !altered) {
            b->appendElements(*originalBSON);
            return;
        }
    }

    // We special case the _id field in top-level objects and move it to the front.
    // This matches other drivers behavior and makes finding the _id field quicker in BSON.
    if (_depth == 0 && hasField("_id")) {
        _writeField(b, "_id", originalBSON);
    }

    enumerate([&](JS::HandleId id) {
        JS::RootedValue x(_context);

        IdWrapper idw(_context, id);

        if (_depth == 0 && idw.isString() && idw.equals("_id"))
            return;

        _writeField(b, id, originalBSON);
    });

    const int sizeWithEOO = b->len() + 1 /*EOO*/ - 4 /*BSONObj::Holder ref count*/;
    uassert(17260,
            str::stream() << "Converting from JavaScript to BSON failed: "
                          << "Object size " << sizeWithEOO << " exceeds limit of "
                          << BSONObjMaxInternalSize << " bytes.",
            sizeWithEOO <= BSONObjMaxInternalSize);
}

void ObjectWrapper::_writeField(BSONObjBuilder* b, Key key, BSONObj* originalParent) {
    JS::RootedValue value(_context);
    key.get(_context, _object, &value);

    ValueWriter x(_context, value, _depth);
    x.setOriginalBSON(originalParent);

    x.writeThis(b, key.toString(_context));
}

}  // namespace mozjs
}  // namespace mongo
