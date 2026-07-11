// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/shell/uri.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/scripting/mozjs/common/internedstring.h"
#include "mongo/scripting/mozjs/common/objectwrapper.h"
#include "mongo/scripting/mozjs/common/valuereader.h"
#include "mongo/scripting/mozjs/common/valuewriter.h"
#include "mongo/scripting/mozjs/common/wrapconstrainedmethod.h"  // IWYU pragma: keep
#include "mongo/scripting/mozjs/shell/implscope.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <js/CallArgs.h>
#include <js/PropertySpec.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

namespace mongo {
namespace mozjs {

const JSFunctionSpec URIInfo::methods[2] = {
    MONGO_ATTACH_JS_CONSTRAINED_METHOD(toString, URIInfo),
    JS_FS_END,
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
    for (const auto& hp : parsed.getServers()) {
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
        optsBuilder.append(kvpair.first.original(), kvpair.second);
    }

    JS::RootedObject thisv(cx);
    auto scope = getScope(cx);
    scope->getProto<URIInfo>().newObject(&thisv);
    ObjectWrapper o(cx, thisv);

    o.setValue(InternedString::uri, uriArg);
    auto& cred = parsed.getCredential();
    if (cred && cred->username) {
        o.setString(InternedString::user, *cred->username);
    }
    if (cred && cred->password) {
        o.setString(InternedString::password, *cred->password);
    }
    o.setBSON(InternedString::options, optsBuilder.obj(), true);
    o.setString(InternedString::database, parsed.getDatabase());
    o.setBoolean(InternedString::isValid, parsed.isValid());
    o.setString(InternedString::setName, parsed.getSetName());
    o.setBSONArray(InternedString::servers, serversBuilder.arr(), true);

    args.rval().setObjectOrNull(thisv);
}

}  // namespace mozjs
}  // namespace mongo
