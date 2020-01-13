
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

#include "mongo/platform/basic.h"

#include "mongo/bson/json.h"
#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_parameters_gen.h"

namespace mongo {

namespace {

class TLSCATrustsSetParameter : public ServerParameter {
public:
    TLSCATrustsSetParameter()
        : ServerParameter(ServerParameterSet::getGlobal(),
                          "tlsCATrusts",
                          true,  // allowedToChangeAtStartup
                          false  // allowedToChangeAtRuntime
                          ) {}

    void append(OperationContext*, BSONObjBuilder&, const std::string&) final;
    Status set(const BSONElement&) final;
    Status setFromString(const std::string&) final;
} tlsCATrustsSetParameter;

void TLSCATrustsSetParameter::append(OperationContext*,
                                     BSONObjBuilder& b,
                                     const std::string& name) {
    if (!sslGlobalParams.tlsCATrusts) {
        b.appendNull(name);
        return;
    }

    BSONArrayBuilder trusts;

    for (const auto& cait : sslGlobalParams.tlsCATrusts.get()) {
        BSONArrayBuilder roles;

        for (const auto& rolename : cait.second) {
            BSONObjBuilder role;
            role.append("role", rolename.getRole());
            role.append("db", rolename.getDB());
            roles.append(role.obj());
        }

        BSONObjBuilder ca;
        ca.append("sha256", cait.first.toHexString());
        ca.append("roles", roles.arr());

        trusts.append(ca.obj());
    }

    b.append(name, trusts.arr());
}

/**
 * tlsCATrusts takes the form of an array of documents describing
 * a set of roles which a given certificate authority may grant.
 *
 * [
 *   {
 *     "sha256": "0123456789abcdef...",   // SHA256 digest of a CA, as hex.
 *     "roles": [                         // Array of grantable RoleNames
 *       { role: "read", db: "foo" },
 *       { role: "readWrite", "db: "bar" },
 *       // etc...
 *     ],
 *   },
 *   // { "sha256": "...", roles: [...]}, // Additional documents...
 * ]
 *
 * If this list has been set, and a client connects with a certificate
 * containing roles which it has not been authorized to grant,
 * then the connection will be refused.
 *
 * Wilcard roles may be defined by omitting the role and/or db portions:
 *
 *   { role: "", db: "foo" }       // May grant any role on the 'foo' DB.
 *   { role: "read", db: "" }      // May grant 'read' role on any DB.
 *   { role: "", db: "" }          // May grant any role on any DB.
 */
Status TLSCATrustsSetParameter::set(const BSONElement& element) try {
    if ((element.type() != Object) || !element.Obj().couldBeArray()) {
        return {ErrorCodes::BadValue, "Value must be an array"};
    }

    SSLParams::TLSCATrusts trusts;
    for (const auto& trustElement : BSONArray(element.Obj())) {
        if (trustElement.type() != Object) {
            return {ErrorCodes::BadValue, "Value must be an array of trust definitions"};
        }

        IDLParserErrorContext ctx("tlsCATrusts");
        auto trust = TLSCATrust::parse(ctx, trustElement.Obj());

        if (trusts.find(trust.getSha256()) != trusts.end()) {
            return {ErrorCodes::BadValue,
                    str::stream() << "Duplicate thumbprint: " << trust.getSha256().toString()};
        }

        const auto& roles = trust.getRoles();
        trusts[std::move(trust.getSha256())] = std::set<RoleName>(roles.begin(), roles.end());
    }

    sslGlobalParams.tlsCATrusts = std::move(trusts);
    return Status::OK();
} catch (...) {
    return exceptionToStatus();
}

Status TLSCATrustsSetParameter::setFromString(const std::string& json) try {
    return set(BSON("" << fromjson(json)).firstElement());
} catch (...) {
    return exceptionToStatus();
}

}  // namespace
}  // namespace mongo
