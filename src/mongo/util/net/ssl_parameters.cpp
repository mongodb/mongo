
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
#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_parameters_gen.h"

namespace mongo {

namespace {

class SSLModeSetting : public ServerParameter {
public:
    SSLModeSetting()
        : ServerParameter(ServerParameterSet::getGlobal(),
                          "sslMode",
                          false,  // allowedToChangeAtStartup
                          true    // allowedToChangeAtRuntime
                          ) {}

    std::string sslModeStr() {
        switch (sslGlobalParams.sslMode.load()) {
            case SSLParams::SSLMode_disabled:
                return "disabled";
            case SSLParams::SSLMode_allowSSL:
                return "allowSSL";
            case SSLParams::SSLMode_preferSSL:
                return "preferSSL";
            case SSLParams::SSLMode_requireSSL:
                return "requireSSL";
            default:
                return "undefined";
        }
    }

    virtual void append(OperationContext* opCtx, BSONObjBuilder& b, const std::string& name) {
        b << name << sslModeStr();
    }

    virtual Status set(const BSONElement& newValueElement) {
        try {
            return setFromString(newValueElement.String());
        } catch (const AssertionException& msg) {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream()
                              << "Invalid value for sslMode via setParameter command: "
                              << newValueElement
                              << ", exception: "
                              << msg.what());
        }
    }

    virtual Status setFromString(const std::string& str) {
#ifndef MONGO_CONFIG_SSL
        return Status(ErrorCodes::IllegalOperation,
                      mongoutils::str::stream()
                          << "Unable to set sslMode, SSL support is not compiled into server");
#endif
        if (str != "disabled" && str != "allowSSL" && str != "preferSSL" && str != "requireSSL") {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream()
                              << "Invalid value for sslMode via setParameter command: "
                              << str);
        }

        int oldMode = sslGlobalParams.sslMode.load();
        if (str == "preferSSL" && oldMode == SSLParams::SSLMode_allowSSL) {
            sslGlobalParams.sslMode.store(SSLParams::SSLMode_preferSSL);
        } else if (str == "requireSSL" && oldMode == SSLParams::SSLMode_preferSSL) {
            sslGlobalParams.sslMode.store(SSLParams::SSLMode_requireSSL);
        } else {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream()
                              << "Illegal state transition for sslMode, attempt to change from "
                              << sslModeStr()
                              << " to "
                              << str);
        }
        return Status::OK();
    }
} sslModeSetting;

class ClusterAuthModeSetting : public ServerParameter {
public:
    ClusterAuthModeSetting()
        : ServerParameter(ServerParameterSet::getGlobal(),
                          "clusterAuthMode",
                          false,  // allowedToChangeAtStartup
                          true    // allowedToChangeAtRuntime
                          ) {}

    std::string clusterAuthModeStr() {
        switch (serverGlobalParams.clusterAuthMode.load()) {
            case ServerGlobalParams::ClusterAuthMode_keyFile:
                return "keyFile";
            case ServerGlobalParams::ClusterAuthMode_sendKeyFile:
                return "sendKeyFile";
            case ServerGlobalParams::ClusterAuthMode_sendX509:
                return "sendX509";
            case ServerGlobalParams::ClusterAuthMode_x509:
                return "x509";
            default:
                return "undefined";
        }
    }

    virtual void append(OperationContext* opCtx, BSONObjBuilder& b, const std::string& name) {
        b << name << clusterAuthModeStr();
    }

    virtual Status set(const BSONElement& newValueElement) {
        try {
            return setFromString(newValueElement.String());
        } catch (const AssertionException& msg) {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream()
                              << "Invalid value for clusterAuthMode via setParameter command: "
                              << newValueElement
                              << ", exception: "
                              << msg.what());
        }
    }

    virtual Status setFromString(const std::string& str) {
#ifndef MONGO_CONFIG_SSL
        return Status(ErrorCodes::IllegalOperation,
                      mongoutils::str::stream() << "Unable to set clusterAuthMode, "
                                                << "SSL support is not compiled into server");
#endif
        if (str != "keyFile" && str != "sendKeyFile" && str != "sendX509" && str != "x509") {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream()
                              << "Invalid value for clusterAuthMode via setParameter command: "
                              << str);
        }

        int oldMode = serverGlobalParams.clusterAuthMode.load();
        int sslMode = sslGlobalParams.sslMode.load();
        if (str == "sendX509" && oldMode == ServerGlobalParams::ClusterAuthMode_sendKeyFile) {
            if (sslMode == SSLParams::SSLMode_disabled || sslMode == SSLParams::SSLMode_allowSSL) {
                return Status(ErrorCodes::BadValue,
                              mongoutils::str::stream()
                                  << "Illegal state transition for clusterAuthMode, "
                                  << "need to enable SSL for outgoing connections");
            }
            serverGlobalParams.clusterAuthMode.store(ServerGlobalParams::ClusterAuthMode_sendX509);
#ifdef MONGO_CONFIG_SSL
            setInternalUserAuthParams(
                BSON(saslCommandMechanismFieldName << "MONGODB-X509" << saslCommandUserDBFieldName
                                                   << "$external"));
#endif
        } else if (str == "x509" && oldMode == ServerGlobalParams::ClusterAuthMode_sendX509) {
            serverGlobalParams.clusterAuthMode.store(ServerGlobalParams::ClusterAuthMode_x509);
        } else {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream()
                              << "Illegal state transition for clusterAuthMode, change from "
                              << clusterAuthModeStr()
                              << " to "
                              << str);
        }
        return Status::OK();
    }
} clusterAuthModeSetting;

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
