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
 * Helper for the JSThread javascript object
 *
 * The workflow is strange because we have a thing in javascript called a
 * JSThread, but we don't actually get to construct it. Instead, we have to
 * inject methods into that thing (via _threadInject) and hang our C++ thread
 * separately (via init() on that type).
 *
 * To manage lifetime, we just add a field into the injected object that's our
 * JSThread and add our holder in as our JSThread's private member.
 */
struct JSThreadInfo : public BaseInfo {
    static void finalize(JSFreeOp* fop, JSObject* obj);

    struct Functions {
        MONGO_DECLARE_JS_FUNCTION(init);
        MONGO_DECLARE_JS_FUNCTION(start);
        MONGO_DECLARE_JS_FUNCTION(join);
        MONGO_DECLARE_JS_FUNCTION(hasFailed);
        MONGO_DECLARE_JS_FUNCTION(returnData);

        MONGO_DECLARE_JS_FUNCTION(_threadInject);
        MONGO_DECLARE_JS_FUNCTION(_scopedThreadInject);
    };

    /**
     * Note that this isn't meant to supply methods for JSThread, it's just
     * there to work with _threadInject. So the name isn't a mistake
     */
    static const JSFunctionSpec threadMethods[6];
    static const JSFunctionSpec freeFunctions[3];

    static const char* const className;
    static const unsigned classFlags = JSCLASS_HAS_PRIVATE;
    static const InstallType installType = InstallType::Private;
};

}  // namespace mozjs
}  // namespace mongo
