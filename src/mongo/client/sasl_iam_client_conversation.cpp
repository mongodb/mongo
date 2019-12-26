/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/client/sasl_iam_client_conversation.h"

#include <string>
#include <time.h>

#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/sasl_iam_client_options.h"
#include "mongo/client/sasl_iam_client_protocol.h"
#include "mongo/client/sasl_iam_client_protocol_gen.h"
#include "mongo/util/net/http_client.h"

namespace mongo {
namespace iam {
SASLIamClientGlobalParams saslIamClientGlobalParams;
}  // namespace iam

std::string getDefaultEC2Host() {
    return iam::saslIamClientGlobalParams.awsEC2InstanceMetadataUrl;
}

std::string getDefaultECSHost() {
    return iam::saslIamClientGlobalParams.awsECSInstanceMetadataUrl;
}

SaslIAMClientConversation::SaslIAMClientConversation(SaslClientSession* saslClientSession)
    : SaslClientConversation(saslClientSession) {}

iam::AWSCredentials SaslIAMClientConversation::_getCredentials() const {

    if (_saslClientSession->hasParameter(SaslClientSession::parameterUser) &&
        _saslClientSession->hasParameter(SaslClientSession::parameterPassword)) {
        return _getUserCredentials();
    } else {
        return _getLocalAWSCredentials();
    }
}

iam::AWSCredentials SaslIAMClientConversation::_getUserCredentials() const {
    if (_saslClientSession->hasParameter(SaslClientSession::parameterIamSessionToken)) {
        return iam::AWSCredentials(
            _saslClientSession->getParameter(SaslClientSession::parameterUser).toString(),
            _saslClientSession->getParameter(SaslClientSession::parameterPassword).toString(),
            _saslClientSession->getParameter(SaslClientSession::parameterIamSessionToken)
                .toString());
    }

    return iam::AWSCredentials(
        _saslClientSession->getParameter(SaslClientSession::parameterUser).toString(),
        _saslClientSession->getParameter(SaslClientSession::parameterPassword).toString());
}

iam::AWSCredentials SaslIAMClientConversation::_getLocalAWSCredentials() const {
    StringData ecsMetadata = getenv("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI");
    if (!ecsMetadata.empty()) {
        return _getEcsCredentials(ecsMetadata);
    }

    return _getEc2Credentials();
}

iam::AWSCredentials SaslIAMClientConversation::_getEc2Credentials() const {
    try {

        std::unique_ptr<HttpClient> httpClient = HttpClient::create();

        // The local web server is just a normal HTTP server
        httpClient->allowInsecureHTTP(true);

        // Retrieve the role attached to the EC2 instance
        DataBuilder getRoleResult =
            httpClient->get(getDefaultEC2Host() + "/latest/meta-data/iam/security-credentials/");

        ConstDataRange cdrRole = getRoleResult.getCursor();
        StringData getRoleOutput;
        cdrRole.readInto<StringData>(&getRoleOutput);

        std::string role = iam::parseRoleFromEC2IamSecurityCredentials(getRoleOutput);

        // Retrieve the temporary credentials of the EC2 instance
        DataBuilder getRoleCredentialsResult = httpClient->get(
            str::stream() << getDefaultEC2Host() + "/latest/meta-data/iam/security-credentials/"
                          << role);

        ConstDataRange cdrCredentials = getRoleCredentialsResult.getCursor();
        StringData getRoleCredentialsOutput;
        cdrCredentials.readInto<StringData>(&getRoleCredentialsOutput);

        return iam::parseCredentialsFromEC2IamSecurityCredentials(getRoleCredentialsOutput);
    } catch (const DBException& e) {
        // Wrap exceptions from HTTP to make them clearer
        uassertStatusOKWithContext(e.toStatus(),
                                   "Failed to retrieve EC2 Instance Metadata Credentials");
    }

    MONGO_UNREACHABLE;
}

iam::AWSCredentials SaslIAMClientConversation::_getEcsCredentials(StringData relativeUri) const {
    try {

        std::unique_ptr<HttpClient> httpClient = HttpClient::create();

        // The local web server is just a normal HTTP server
        httpClient->allowInsecureHTTP(true);

        // Retrieve the security token attached to the ECS task
        DataBuilder getRoleResult = httpClient->get(getDefaultECSHost() + relativeUri);

        ConstDataRange cdrRole = getRoleResult.getCursor();
        StringData getRoleOutput;
        cdrRole.readInto<StringData>(&getRoleOutput);

        return iam::parseCredentialsFromECSTaskIamCredentials(getRoleOutput);
    } catch (const DBException& e) {
        // Wrap exceptions from HTTP to make them clearer
        uassertStatusOKWithContext(e.toStatus(),
                                   "Failed to retrieve ECS Instance Metadata Credentials");
    }

    MONGO_UNREACHABLE;
}

StatusWith<bool> SaslIAMClientConversation::step(StringData inputData, std::string* outputData) {
    if (_step > 2) {
        return Status(ErrorCodes::AuthenticationFailed,
                      str::stream() << "Invalid IAM authentication step: " << _step);
    }

    _step++;

    try {
        if (_step == 1) {
            return _firstStep(outputData);
        }

        return _secondStep(inputData, outputData);
    } catch (...) {
        return exceptionToStatus();
    }
}

StatusWith<bool> SaslIAMClientConversation::_firstStep(std::string* outputData) {

    *outputData = iam::generateClientFirst(&_clientNonce);

    return false;
}

StatusWith<bool> SaslIAMClientConversation::_secondStep(StringData inputData,
                                                        std::string* outputData) {
    auto credentials = _getCredentials();

    *outputData = iam::generateClientSecond(inputData, _clientNonce, credentials);

    return true;
}

}  // namespace mongo
