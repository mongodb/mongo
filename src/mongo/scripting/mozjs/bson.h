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

#pragma once

#include <tuple>

#include "mongo/db/jsobj.h"
#include "mongo/scripting/mozjs/wraptype.h"

namespace mongo {
namespace mozjs {

/**
 * Provides a wrapper for BSONObj's in JS. The main idea here is that BSONObj's
 * can be read only, or read write when we shim them in, and in all cases we
 * lazily load their member elements into JS. So a bunch of these lifecycle
 * methods are set up to wrap field access (enumerate, resolve and
 * del/setProperty).
 *
 * Note that installType is private. So you can only get BSON types in JS via
 * ::make() from C++.
 */
struct BSONInfo : public BaseInfo {
    static void delProperty(JSContext* cx,
                            JS::HandleObject obj,
                            JS::HandleId id,
                            JS::ObjectOpResult& result);
    static void enumerate(JSContext* cx,
                          JS::HandleObject obj,
                          JS::AutoIdVector& properties,
                          bool enumerableOnly);
    static void finalize(JSFreeOp* fop, JSObject* obj);
    static void resolve(JSContext* cx, JS::HandleObject obj, JS::HandleId id, bool* resolvedp);
    static void setProperty(JSContext* cx,
                            JS::HandleObject obj,
                            JS::HandleId id,
                            JS::MutableHandleValue vp,
                            JS::ObjectOpResult& result);

    static const char* const className;
    static const unsigned classFlags = JSCLASS_HAS_PRIVATE;
    static const InstallType installType = InstallType::Private;
    static void postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto);

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(bsonWoCompare);
    };

    static const JSFunctionSpec freeFunctions[2];

    static std::tuple<BSONObj*, bool> originalBSON(JSContext* cx, JS::HandleObject obj);
    static void make(
        JSContext* cx, JS::MutableHandleObject obj, BSONObj bson, const BSONObj* parent, bool ro);
};

}  // namespace mozjs
}  // namespace mongo
