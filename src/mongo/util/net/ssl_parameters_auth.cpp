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

#include "mongo/client/authenticate.h"
#include "mongo/config.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/server_options.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_parameters_auth_gen.h"

namespace mongo {
namespace {

std::string clusterAuthModeFormat() {
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
            // Default case because clusterAuthMode is an AtomicWord<int> and not bound by enum
            // rules.
            return "undefined";
    }
}

StatusWith<ServerGlobalParams::ClusterAuthModes> clusterAuthModeParse(StringData strMode) {
    if (strMode == "keyFile") {
        return ServerGlobalParams::ClusterAuthMode_keyFile;
    } else if (strMode == "sendKeyFile") {
        return ServerGlobalParams::ClusterAuthMode_sendKeyFile;
    } else if (strMode == "sendX509") {
        return ServerGlobalParams::ClusterAuthMode_sendX509;
    } else if (strMode == "x509") {
        return ServerGlobalParams::ClusterAuthMode_x509;
    } else {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Invalid clusterAuthMode '" << strMode
                          << "', expected one of: 'keyFile', 'sendKeyFile', 'sendX509', or 'x509'");
    }
}

}  // namespace
void ClusterAuthModeServerParameter::append(OperationContext*,
                                            BSONObjBuilder& builder,
                                            const std::string& fieldName) {
    builder.append(fieldName, clusterAuthModeFormat());
}

Status ClusterAuthModeServerParameter::setFromString(const std::string& strMode) {

    auto swMode = clusterAuthModeParse(strMode);
    if (!swMode.isOK()) {
        return swMode.getStatus();
    }

    auto mode = swMode.getValue();
    auto oldMode = serverGlobalParams.clusterAuthMode.load();
    auto sslMode = sslGlobalParams.sslMode.load();
    if ((mode == ServerGlobalParams::ClusterAuthMode_sendX509) &&
        (oldMode == ServerGlobalParams::ClusterAuthMode_sendKeyFile)) {
        if (sslMode == SSLParams::SSLMode_disabled || sslMode == SSLParams::SSLMode_allowSSL) {
            return {ErrorCodes::BadValue,
                    "Illegal state transition for clusterAuthMode, need to enable SSL for outgoing "
                    "connections"};
        }
        serverGlobalParams.clusterAuthMode.store(mode);
        auth::setInternalUserAuthParams(BSON(saslCommandMechanismFieldName
                                             << "MONGODB-X509" << saslCommandUserDBFieldName
                                             << "$external"));
    } else if ((mode == ServerGlobalParams::ClusterAuthMode_x509) &&
               (oldMode == ServerGlobalParams::ClusterAuthMode_sendX509)) {
        serverGlobalParams.clusterAuthMode.store(mode);
    } else {
        return {ErrorCodes::BadValue,
                str::stream() << "Illegal state transition for clusterAuthMode, change from "
                              << clusterAuthModeFormat() << " to " << strMode};
    }

    return Status::OK();
}

}  // namespace mongo
