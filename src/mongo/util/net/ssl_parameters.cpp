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

#include "mongo/util/net/ssl_parameters.h"

#include "mongo/bson/json.h"
#include "mongo/config.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_parameters_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {
namespace {

template <typename T, typename U>
StatusWith<SSLParams::SSLModes> checkTLSModeTransition(T modeToString,
                                                       U stringToMode,
                                                       StringData parameterName,
                                                       StringData strMode) {
    auto mode = stringToMode(strMode);
    if (!mode.isOK()) {
        return mode.getStatus();
    }
    auto oldMode = sslGlobalParams.sslMode.load();
    if ((mode == SSLParams::SSLMode_preferSSL) && (oldMode == SSLParams::SSLMode_allowSSL)) {
        return mode;
    } else if ((mode == SSLParams::SSLMode_requireSSL) &&
               (oldMode == SSLParams::SSLMode_preferSSL)) {
        return mode;
    } else {
        return {ErrorCodes::BadValue,
                str::stream() << "Illegal state transition for " << parameterName
                              << ", attempt to change from "
                              << modeToString(static_cast<SSLParams::SSLModes>(oldMode)) << " to "
                              << strMode};
    }
}

std::once_flag warnForSSLMode;

}  // namespace

void SSLModeServerParameter::append(OperationContext*,
                                    BSONObjBuilder* builder,
                                    StringData fieldName,
                                    const boost::optional<TenantId>&) {
    std::call_once(warnForSSLMode, [] {
        LOGV2_WARNING(
            23803, "Use of deprecated server parameter 'sslMode', please use 'tlsMode' instead.");
    });

    builder->append(fieldName, SSLParams::sslModeFormat(sslGlobalParams.sslMode.load()));
}

void TLSModeServerParameter::append(OperationContext*,
                                    BSONObjBuilder* builder,
                                    StringData fieldName,
                                    const boost::optional<TenantId>&) {
    builder->append(
        fieldName,
        SSLParams::tlsModeFormat(static_cast<SSLParams::SSLModes>(sslGlobalParams.sslMode.load())));
}

Status SSLModeServerParameter::setFromString(StringData strMode, const boost::optional<TenantId>&) {
    std::call_once(warnForSSLMode, [] {
        LOGV2_WARNING(
            23804, "Use of deprecated server parameter 'sslMode', please use 'tlsMode' instead.");
    });

    auto swNewMode = checkTLSModeTransition(
        SSLParams::sslModeFormat, SSLParams::sslModeParse, "sslMode", strMode);
    if (!swNewMode.isOK()) {
        return swNewMode.getStatus();
    }
    sslGlobalParams.sslMode.store(swNewMode.getValue());
    return Status::OK();
}

Status TLSModeServerParameter::setFromString(StringData strMode, const boost::optional<TenantId>&) {
    auto swNewMode = checkTLSModeTransition(
        SSLParams::tlsModeFormat, SSLParams::tlsModeParse, "tlsMode", strMode);
    if (!swNewMode.isOK()) {
        return swNewMode.getStatus();
    }
    sslGlobalParams.sslMode.store(swNewMode.getValue());
    return Status::OK();
}

void TLSCATrustsSetParameter::append(OperationContext*,
                                     BSONObjBuilder* b,
                                     StringData name,
                                     const boost::optional<TenantId>&) {
    if (!sslGlobalParams.tlsCATrusts) {
        b->appendNull(name);
        return;
    }

    BSONArrayBuilder trusts;

    for (const auto& cait : sslGlobalParams.tlsCATrusts.value()) {
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

    b->append(name, trusts.arr());
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
Status TLSCATrustsSetParameter::set(const BSONElement& element,
                                    const boost::optional<TenantId>&) try {
    if ((element.type() != Object) || !element.Obj().couldBeArray()) {
        return {ErrorCodes::BadValue, "Value must be an array"};
    }

    SSLParams::TLSCATrusts trusts;
    for (const auto& trustElement : BSONArray(element.Obj())) {
        if (trustElement.type() != Object) {
            return {ErrorCodes::BadValue, "Value must be an array of trust definitions"};
        }

        IDLParserContext ctx("tlsCATrusts");
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

Status TLSCATrustsSetParameter::setFromString(StringData json,
                                              const boost::optional<TenantId>&) try {
    return set(BSON("" << fromjson(json)).firstElement(), boost::none);
} catch (...) {
    return exceptionToStatus();
}

}  // namespace mongo

mongo::Status mongo::validateOpensslCipherConfig(const std::string&,
                                                 const boost::optional<TenantId>&) {
    if (sslGlobalParams.sslCipherConfig != kSSLCipherConfigDefault) {
        return {ErrorCodes::BadValue,
                "opensslCipherConfig setParameter is incompatible with net.tls.tlsCipherConfig"};
    }
    // Note that there is very little validation that we can do here.
    // OpenSSL exposes no API to validate a cipher config string. The only way to figure out
    // what a string maps to is to make an SSL_CTX object, set the string on it, then parse the
    // resulting STACK_OF object. If provided an invalid entry in the string, it will silently
    // ignore it. Because an entry in the string may map to multiple ciphers, or remove ciphers
    // from the final set produced by the full string, we can't tell if any entry failed
    // to parse.
    return Status::OK();
}

mongo::Status mongo::validateDisableNonTLSConnectionLogging(const bool&,
                                                            const boost::optional<TenantId>&) {
    if (sslGlobalParams.disableNonSSLConnectionLoggingSet) {
        return {ErrorCodes::BadValue,
                "Error parsing command line: Multiple occurrences of option "
                "disableNonTLSConnectionLogging"};
    }
    return Status::OK();
}

mongo::Status mongo::onUpdateDisableNonTLSConnectionLogging(const bool&) {
    // disableNonSSLConnectionLogging is a write-once setting.
    // Once we've updated it, we're not allowed to specify the set-param again.
    // Record that update in a second bool value.
    sslGlobalParams.disableNonSSLConnectionLoggingSet = true;
    return Status::OK();
}
