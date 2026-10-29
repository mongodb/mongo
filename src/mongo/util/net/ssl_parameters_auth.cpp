// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/client/authenticate.h"
#include "mongo/config.h"
#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_parameters_auth_gen.h"

#include <string_view>

namespace mongo {
void ClusterAuthModeServerParameter::append(OperationContext*,
                                            BSONObjBuilder* builder,
                                            std::string_view fieldName,
                                            const boost::optional<TenantId>&) {
    const auto clusterAuthMode = ClusterAuthMode::get(getGlobalServiceContext());
    builder->append(fieldName, clusterAuthMode.toString());
}

Status ClusterAuthModeServerParameter::setFromString(std::string_view strMode,
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
        auth::setInternalUserAuthParams(auth::createInternalX509AuthCredential());
    }

    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

}  // namespace mongo
