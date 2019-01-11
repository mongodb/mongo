
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

#pragma once

#include <jsapi.h>
#include <string>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/decimal128.h"
#include "mongo/scripting/mozjs/exception.h"
#include "mongo/scripting/mozjs/internedstring.h"
#include "mongo/scripting/mozjs/jsstringwrapper.h"
#include "mongo/scripting/mozjs/lifetimestack.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;
class BSONElement;

namespace mozjs {

class MozJSImplScope;
class ValueWriter;

/**
 * Wraps JSObject's with helpers for accessing their properties
 *
 * This wraps a RootedObject, so should only be allocated on the stack and is
 * not movable or copyable
 */
class ObjectWrapper {
    friend class ValueWriter;

public:
    /**
     * Helper subclass that provides some easy boilerplate for accessing
     * properties by string, index or id.
     */
    class Key {
        friend class ObjectWrapper;

        enum class Type : char {
            Field,
            Index,
            Id,
            InternedString,
        };

    public:
        Key(const char* field) : _field(field), _type(Type::Field) {}
        Key(uint32_t idx) : _idx(idx), _type(Type::Index) {}
        Key(JS::HandleId id) : _id(id), _type(Type::Id) {}
        Key(InternedString id) : _internedString(id), _type(Type::InternedString) {}

    private:
        void get(JSContext* cx, JS::HandleObject o, JS::MutableHandleValue value);
        void set(JSContext* cx, JS::HandleObject o, JS::HandleValue value);
        bool has(JSContext* cx, JS::HandleObject o);
        bool hasOwn(JSContext* cx, JS::HandleObject o);
        void define(
            JSContext* cx, JS::HandleObject o, unsigned attrs, JSNative getter, JSNative setter);
        void define(JSContext* cx, JS::HandleObject o, JS::HandleValue value, unsigned attrs);
        void del(JSContext* cx, JS::HandleObject o);
        std::string toString(JSContext* cx);
        StringData toStringData(JSContext* cx, JSStringWrapper* jsstr);

        union {
            const char* _field;
            uint32_t _idx;
            jsid _id;
            InternedString _internedString;
        };
        Type _type;
    };

    ObjectWrapper(JSContext* cx, JS::HandleObject obj);
    ObjectWrapper(JSContext* cx, JS::HandleValue value);

    double getNumber(Key key);
    int getNumberInt(Key key);
    long long getNumberLongLong(Key key);
    Decimal128 getNumberDecimal(Key key);
    std::string getString(Key key);
    bool getBoolean(Key key);
    BSONObj getObject(Key key);
    void getValue(Key key, JS::MutableHandleValue value);

    void setNumber(Key key, double val);
    void setString(Key key, StringData val);
    void setBoolean(Key key, bool val);
    void setBSONElement(Key key, const BSONElement& elem, const BSONObj& obj, bool readOnly);
    void setBSON(Key key, const BSONObj& obj, bool readOnly);
    void setBSONArray(Key key, const BSONObj& obj, bool readOnly);
    void setValue(Key key, JS::HandleValue value);
    void setObject(Key key, JS::HandleObject value);
    void setPrototype(JS::HandleObject value);

    /**
     * See JS_DefineProperty for what sort of attributes might be useful
     */
    void defineProperty(Key key, unsigned attrs, JSNative getter, JSNative setter);
    void defineProperty(Key key, JS::HandleValue value, unsigned attrs);

    void deleteProperty(Key key);

    /**
     * Returns the bson type of the property
     */
    int type(Key key);

    void rename(Key key, const char* to);

    bool hasField(Key key);
    bool hasOwnField(Key key);

    void callMethod(const char* name, const JS::HandleValueArray& args, JS::MutableHandleValue out);
    void callMethod(const char* name, JS::MutableHandleValue out);
    void callMethod(JS::HandleValue fun,
                    const JS::HandleValueArray& args,
                    JS::MutableHandleValue out);
    void callMethod(JS::HandleValue fun, JS::MutableHandleValue out);

    /**
     * Safely enumerates fields in the object, invoking a callback for each id
     */
    template <typename T>
    void enumerate(T&& callback) {
        JS::Rooted<JS::IdVector> ids(_context, JS::IdVector(_context));

        if (!JS_Enumerate(_context, _object, &ids))
            throwCurrentJSException(
                _context, ErrorCodes::JSInterpreterFailure, "Failure to enumerate object");

        JS::RootedId rid(_context);
        for (size_t i = 0; i < ids.length(); ++i) {
            rid.set(ids[i]);
            if (!callback(rid))
                break;
        }
    }

    /**
     * Writes a bson object reflecting the contents of the object
     */
    BSONObj toBSON();

    JS::HandleObject thisv() {
        return _object;
    }

    std::string getClassName();

private:
    /**
     * The maximum depth of recursion for writeField
     */
    static const int kMaxWriteFieldDepth = 150;

    /**
     * The state needed to write a single level of a nested javascript object as a
     * bson object.
     *
     * We use this between ObjectWrapper and ValueWriter to avoid recursion in
     * translating js to bson.
     */
    struct WriteFieldRecursionFrame {
        WriteFieldRecursionFrame(JSContext* cx,
                                 JSObject* obj,
                                 BSONObjBuilder* parent,
                                 StringData sd);

        BSONObjBuilder* subbob_or(BSONObjBuilder* option) {
            return subbob ? &subbob.get() : option;
        }

        JS::RootedObject thisv;

        // ids for the keys of thisv
        JS::Rooted<JS::IdVector> ids;

        // Current index of the current key we're working on
        std::size_t idx = 0;

        boost::optional<BSONObjBuilder> subbob;
        BSONObj* originalBSON = nullptr;
        bool altered;
    };

    /**
     * Synthetic stack of variables for writeThis
     *
     * We use a LifetimeStack here because we have SpiderMonkey Rooting types which
     * are non-copyable and non-movable and have to be on the stack.
     */
    using WriteFieldRecursionFrames = LifetimeStack<WriteFieldRecursionFrame, kMaxWriteFieldDepth>;

    /**
     * writes the field "key" into the associated builder
     *
     * optional originalBSON is used to track updates to types (NumberInt
     * overwritten by a float, but coercible to the original type, etc.)
     */
    void _writeField(BSONObjBuilder* b,
                     Key key,
                     WriteFieldRecursionFrames* frames,
                     BSONObj* originalBSON);

    JSContext* _context;
    JS::RootedObject _object;
};

}  // namespace mozjs
}  // namespace mongo
