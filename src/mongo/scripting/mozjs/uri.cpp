/**
 * Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/scripting/mozjs/uri.h"

#include <algorithm>
#include <iterator>

#include "mongo/client/mongo_uri.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"
#include "mongo/scripting/mozjs/wrapconstrainedmethod.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace mozjs {

const JSFunctionSpec URIInfo::methods[2] = {
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(toString, URIInfo), JS_FS_END,
};

const char* const URIInfo::className = "MongoURI";

void URIInfo::Functions::toString::call(JSContext* cx, JS::CallArgs args) {
    ObjectWrapper o(cx, args.thisv());
    ValueReader(cx, args.rval()).fromStringData(o.getString(InternedString::uri));
}

void URIInfo::construct(JSContext* cx, JS::CallArgs args) {
    uassert(ErrorCodes::BadValue, "MongoURI needs 1 argument", args.length() == 1);

    JS::HandleValue uriArg = args.get(0);
    if (!uriArg.isString())
        uasserted(ErrorCodes::BadValue, "uri must be a string");

    std::string uri = ValueWriter(cx, args.get(0)).toString();

    auto sw = MongoURI::parse(uri);
    auto parsed = uassertStatusOK(sw);

    BSONArrayBuilder serversBuilder;
    for (const auto hp : parsed.getServers()) {
        BSONObjBuilder b;
        b.append("server", hp.toString());
        b.append("host", hp.host());
        if (hp.hasPort()) {
            b.append("port", hp.port());
        }
        serversBuilder.append(b.obj());
    }

    BSONObjBuilder optsBuilder;
    for (const auto& kvpair : parsed.getOptions()) {
        optsBuilder.append(kvpair.first, kvpair.second);
    }

    JS::RootedObject thisv(cx);
    auto scope = getScope(cx);
    scope->getProto<URIInfo>().newObject(&thisv);
    ObjectWrapper o(cx, thisv);

    o.setValue(InternedString::uri, uriArg);
    o.setString(InternedString::user, parsed.getUser());
    o.setString(InternedString::password, parsed.getPassword());
    o.setBSON(InternedString::options, optsBuilder.obj(), true);
    o.setString(InternedString::database, parsed.getDatabase());
    o.setBoolean(InternedString::isValid, parsed.isValid());
    o.setString(InternedString::setName, parsed.getSetName());
    o.setBSONArray(InternedString::servers, serversBuilder.arr(), true);

    args.rval().setObjectOrNull(thisv);
}

}  // namespace mozjs
}  // namespace mongo
