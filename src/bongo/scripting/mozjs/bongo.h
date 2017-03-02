/**
 * Copyright (C) 2015 BongoDB Inc.
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

#include "bongo/scripting/mozjs/wraptype.h"

namespace bongo {
namespace mozjs {

/**
 * Shared code for the "Bongo" javascript object.
 *
 * The idea here is that there is a lot of shared functionality between the
 * "Bongo" we see in the shell and the "Bongo" in dbeval.  So we provide one
 * info type with common code and differentiate with varying constructors.
 */
struct BongoBase : public BaseInfo {
    static void finalize(JSFreeOp* fop, JSObject* obj);

    struct Functions {
        BONGO_DECLARE_JS_FUNCTION(auth);
        BONGO_DECLARE_JS_FUNCTION(copyDatabaseWithSCRAM);
        BONGO_DECLARE_JS_FUNCTION(close);
        BONGO_DECLARE_JS_FUNCTION(cursorFromId);
        BONGO_DECLARE_JS_FUNCTION(cursorHandleFromId);
        BONGO_DECLARE_JS_FUNCTION(find);
        BONGO_DECLARE_JS_FUNCTION(getClientRPCProtocols);
        BONGO_DECLARE_JS_FUNCTION(getServerRPCProtocols);
        BONGO_DECLARE_JS_FUNCTION(insert);
        BONGO_DECLARE_JS_FUNCTION(isReplicaSetConnection);
        BONGO_DECLARE_JS_FUNCTION(logout);
        BONGO_DECLARE_JS_FUNCTION(remove);
        BONGO_DECLARE_JS_FUNCTION(runCommand);
        BONGO_DECLARE_JS_FUNCTION(runCommandWithMetadata);
        BONGO_DECLARE_JS_FUNCTION(setClientRPCProtocols);
        BONGO_DECLARE_JS_FUNCTION(update);
        BONGO_DECLARE_JS_FUNCTION(getMinWireVersion);
        BONGO_DECLARE_JS_FUNCTION(getMaxWireVersion);
    };

    static const JSFunctionSpec methods[19];

    static const char* const className;
    static const unsigned classFlags = JSCLASS_HAS_PRIVATE;
};

/**
 * The dbeval variant of "Bongo"
 */
struct BongoLocalInfo : public BongoBase {
    static void construct(JSContext* cx, JS::CallArgs args);
};

/**
 * The shell variant of "Bongo"
 */
struct BongoExternalInfo : public BongoBase {
    static void construct(JSContext* cx, JS::CallArgs args);

    struct Functions {
        BONGO_DECLARE_JS_FUNCTION(_forgetReplSet);
        BONGO_DECLARE_JS_FUNCTION(load);
        BONGO_DECLARE_JS_FUNCTION(quit);
    };

    static const JSFunctionSpec freeFunctions[4];
};

}  // namespace mozjs
}  // namespace bongo
