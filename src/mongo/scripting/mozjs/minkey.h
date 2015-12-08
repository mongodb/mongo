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

#include "mongo/scripting/mozjs/wraptype.h"

namespace mongo {
namespace mozjs {

/**
 * The "MinKey" Javascript object.
 *
 * These are slightly special, in that there is only one MinKey object and
 * whenever you call the constructor to make a new one you just get the
 * "singleton" MinKey from the prototype. See the postInstall for details.
 */
struct MinKeyInfo : public BaseInfo {
    static void call(JSContext* cx, JS::CallArgs args);
    static void construct(JSContext* cx, JS::CallArgs args);
    static void hasInstance(JSContext* cx,
                            JS::HandleObject obj,
                            JS::MutableHandleValue vp,
                            bool* bp);

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(tojson);
    };

    static const JSFunctionSpec methods[2];

    static void postInstall(JSContext* cx, JS::HandleObject global, JS::HandleObject proto);

    static const char* const className;
};

}  // namespace mozjs
}  // namespace mongo
