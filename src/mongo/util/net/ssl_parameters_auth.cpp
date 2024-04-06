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
#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/server_options.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_parameters_auth_gen.h"

namespace mongo {
void ClusterAuthModeServerParameter::append(OperationContext*,
                                            BSONObjBuilder* builder,
                                            StringData fieldName,
                                            const boost::optional<TenantId>&) {
    const auto clusterAuthMode = ClusterAuthMode::get(getGlobalServiceContext());
    builder->append(fieldName, clusterAuthMode.toString());
}

Status ClusterAuthModeServerParameter::setFromString(StringData strMode,
                                                     const boost::optional<TenantId>&) try {
    auto mode = uassertStatusOK(ClusterAuthMode::parse(strMode));

    auto sslMode = sslGlobalParams.sslMode.load();
    if (mode.allowsX509()) {
        if (sslMode == SSLParams::SSLMode_disabled || sslMode == SSLParams::SSLMode_allowSSL) {
            return {ErrorCodes::BadValue,
                    "Illegal state transition for clusterAuthMode, need to enable SSL for outgoing "
                    "connections"};
        }
    }

    // Set our ingress mode, then our egress parameters.
    ClusterAuthMode::set(getGlobalServiceContext(), mode);
    if (mode.sendsX509()) {
        auth::setInternalUserAuthParams(auth::createInternalX509AuthDocument());
    }

    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

}  // namespace mongo
