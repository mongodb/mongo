/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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


#include "mongo/db/auth/sasl_x509_server_conversation.h"

#include "mongo/db/auth/auth_options_gen.h"
#include "mongo/db/auth/authentication_session.h"
#include "mongo/db/auth/authorization_manager_global_parameters_gen.h"
#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/user_request_x509.h"
#include "mongo/db/auth/x509_protocol_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/net/ssl_types.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

namespace mongo::auth {

namespace {
GlobalSASLMechanismRegisterer<X509ServerFactory> x509Registerer;

constexpr auto kX509AuthenticationDisabledMessage = "x.509 authentication is disabled."_sd;

std::string unpackName(StringData inputData) {
    if (inputData.empty()) {
        return "";
    }

    ConstDataRange cdr(inputData.data(), inputData.size());
    auto payload = cdr.read<Validated<BSONObj>>().val;

    auto request =
        X509MechanismClientStep1::parse(payload, IDLParserContext{"x509-authentication"});
    const auto& user = request.getPrincipalName();

    return std::string{user.value_or("")};
}

/**
 * GetUserName should:
 *
 * 1. Unpack the inputData field.
 * 2. Compare the user name to the subject DN from SSLPeerInfo.
 */
std::string getUserName(Client* client,
                        StringData inputData,
                        std::shared_ptr<const SSLPeerInfo> sslPeerInfo) {
    uassert(ErrorCodes::AuthenticationFailed, "No SSLPeerInfo available", sslPeerInfo);

    const auto& clientName = sslPeerInfo->subjectName();
    uassert(ErrorCodes::AuthenticationFailed,
            "No verified subject name available from client",
            !clientName.empty());

    auto user = unpackName(inputData);

    if (user.empty()) {
        return clientName.toString();
    }

    uassert(ErrorCodes::AuthenticationFailed,
            "There is no x.509 client certificate matching the user.",
            user == clientName.toString());

    return user;
}

}  // namespace

StatusWith<std::unique_ptr<UserRequest>> SaslX509ServerMechanism::makeUserRequest(
    OperationContext* opCtx) const {
    std::unique_ptr<UserRequest> request = std::make_unique<UserRequestGeneral>(
        UserName(getPrincipalName(), getAuthenticationDatabase()), boost::none);

    if (!opCtx || !opCtx->getClient()) {
        // Without an opCtx, we have no client, and none of the paths to
        // acquiring roles below will succeed.
        return std::move(request);
    }

    auto session = opCtx->getClient()->session();

    if (isClusterMember(opCtx->getClient())) {
        return std::make_unique<UserRequestGeneral>((*internalSecurity.getUser())->getName(),
                                                    boost::none);
    }

    if (!allowRolesFromX509Certificates || !session) {
        return std::move(request);
    }

    auto sslPeerInfo = SSLPeerInfo::forSession(session);

    if (!sslPeerInfo || sslPeerInfo->roles().empty() ||
        (sslPeerInfo->subjectName().toString() != request->getUserName().getUser())) {
        return std::move(request);
    }

    const auto& peerRoles = sslPeerInfo->roles();
    std::set<RoleName> requestRoles;
    std::copy(
        peerRoles.begin(), peerRoles.end(), std::inserter(requestRoles, requestRoles.begin()));

    return UserRequestX509::makeUserRequestX509(
        UserName(getPrincipalName(), getAuthenticationDatabase()),
        std::move(requestRoles),
        sslPeerInfo);
}

bool SaslX509ServerMechanism::isClusterMember(Client* client) const {
    // If the server is shutting down while the authentication session is running, we can trigger
    // a call to isClusterMember without an actual client. This would cause the server to crash
    // on shutdown, necessitating the check below.
    if (!client) {
        return false;
    }

    const auto clusterAuthMode = ClusterAuthMode::get(client->getServiceContext());

    if (!clusterAuthMode.allowsX509()) {
        return false;
    }

    std::shared_ptr<transport::Session> session = client->session();
    if (!session || !session->getSSLConfiguration()) {
        return false;
    }

    auto sslPeerInfo = SSLPeerInfo::forSession(session);
    auto clusterMembership = sslPeerInfo ? sslPeerInfo->getClusterMembership() : boost::none;
    if (!session->getSSLConfiguration()->isClusterMember(this->getPrincipalName(),
                                                         clusterMembership)) {
        return false;
    }

    return true;
}

/**
 * The steps for X509 SASL are described below.
 *
 * 1. We should update the step count for SASL.
 * 2. We should get the correct UserName from SSLPeerInfo and update _principalName.
 * 3. We should exit early if we are performing client-server auth.
 * 4. We should check that we are correctly authorizing cluster users.
 */
StatusWith<std::tuple<bool, std::string>> SaslX509ServerMechanism::stepImpl(
    OperationContext* opCtx, StringData inputData) try {
    _step++;

    // We should update the step count for SASL.
    if (_step > kMaxStep || _step <= 0) {
        return Status(ErrorCodes::AuthenticationFailed,
                      str::stream() << "Invalid X509 authentication step: " << _step);
    }

    uassert(ErrorCodes::BadValue,
            kX509AuthenticationDisabledMessage + ": Feature Flag is disabled.",
            gFeatureFlagRearchitectUserAcquisition.isEnabled());

    // We want to set the _principalName here in case we need it for auditing purposes.
    ServerMechanismBase::_principalName = unpackName(inputData);

    // We should get the correct UserName from SSLPeerInfo.
    auto client = opCtx->getClient();
    auto sslPeerInfo = SSLPeerInfo::forSession(client->session());
    ServerMechanismBase::_principalName = getUserName(client, inputData, sslPeerInfo);

    uassert(ErrorCodes::BadValue,
            "MONGODB-X509 is only available on the '$external' database.",
            ServerMechanismBase::getAuthenticationDatabase() == "$external");

    if (!isClusterMember(opCtx->getClient())) {
        return std::make_tuple(true, std::string());
    }

    const auto clusterAuthMode = ClusterAuthMode::get(opCtx->getServiceContext());

    // Handle internal cluster member auth, only applies to server-server connections
    if (!clusterAuthMode.allowsX509() && gEnforceUserClusterSeparation) {
        return Status(ErrorCodes::AuthenticationFailed,
                      "The provided certificate can only be used for cluster authentication, not "
                      "client authentication. The current configuration does not allow x.509 "
                      "cluster authentication, check the --clusterAuthMode flag");
    }

    if (!client->isPossiblyUnauthenticatedInternalClient()) {
        LOGV2_WARNING(8209200,
                      "Client isn't a mongod or mongos, but is connecting with a certificate "
                      "with cluster membership");
    }

    auto sslConfiguration = client->session()->getSSLConfiguration();
    if (gEnforceUserClusterSeparation && sslConfiguration->isClusterExtensionSet()) {
        auto* am = AuthorizationManager::get(opCtx->getService());
        BSONObj ignored;

        UserName username(ServerMechanismBase::_principalName,
                          ServerMechanismBase::getAuthenticationDatabase());

        // At this point, we know that the X.509 subject DN meets the criteria for cluster
        // membership. Since gEnforceUserClusterSeparation is set, we must check whether a user with
        // the same username as the X.509 subject DN has already been created.
        // SaslX509Mechanism::makeUserRequest() automatically transforms X.509 UserRequests that
        // satisfy cluster membership into UserRequestGeneral instances that represent
        // local.__system. Passing that UserRequest into AuthorizationManager::acquireUser() will
        // always succeed with the internal user. Therefore, we make an X.509 UserRequest via
        // UserRequestX509::makeUserRequestX509 so that we are checking for the X.509 user rather
        // than local.__system.
        auto userRequest = uassertStatusOK(
            UserRequestX509::makeUserRequestX509(username, boost::none, sslPeerInfo));
        bool userExists = am->acquireUser(opCtx, std::move(userRequest)).isOK();
        uassert(ErrorCodes::AuthenticationFailed,
                "The provided certificate represents both a cluster member and an "
                "explicit user which exists in the authzn database. "
                "Prohibiting authentication due to enforceUserClusterSeparation setting.",
                !userExists);
    }

    return std::make_tuple(true, std::string());
} catch (DBException& ex) {
    return ex.toStatus();
}

}  // namespace mongo::auth
