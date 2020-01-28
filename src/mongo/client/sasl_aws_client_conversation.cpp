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

#include "mongo/client/sasl_aws_client_conversation.h"

#include <string>
#include <time.h>

#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/sasl_aws_client_options.h"
#include "mongo/client/sasl_aws_client_protocol.h"
#include "mongo/client/sasl_aws_client_protocol_gen.h"
#include "mongo/util/net/http_client.h"

namespace mongo {
namespace awsIam {
SASLAwsClientGlobalParams saslAwsClientGlobalParams;
}  // namespace awsIam

namespace {
std::string getDefaultEC2Host() {
    return awsIam::saslAwsClientGlobalParams.awsEC2InstanceMetadataUrl;
}

std::string getDefaultECSHost() {
    return awsIam::saslAwsClientGlobalParams.awsECSInstanceMetadataUrl;
}

StringData toString(DataBuilder& builder) {
    ConstDataRange cdr = builder.getCursor();
    StringData str;
    cdr.readInto<StringData>(&str);
    return str;
}
}  // namespace

SaslAWSClientConversation::SaslAWSClientConversation(SaslClientSession* saslClientSession)
    : SaslClientConversation(saslClientSession) {}

awsIam::AWSCredentials SaslAWSClientConversation::_getCredentials() const {

    if (_saslClientSession->hasParameter(SaslClientSession::parameterUser) &&
        _saslClientSession->hasParameter(SaslClientSession::parameterPassword)) {
        return _getUserCredentials();
    } else {
        return _getLocalAWSCredentials();
    }
}

awsIam::AWSCredentials SaslAWSClientConversation::_getUserCredentials() const {
    if (_saslClientSession->hasParameter(SaslClientSession::parameterAWSSessionToken)) {
        return awsIam::AWSCredentials(
            _saslClientSession->getParameter(SaslClientSession::parameterUser).toString(),
            _saslClientSession->getParameter(SaslClientSession::parameterPassword).toString(),
            _saslClientSession->getParameter(SaslClientSession::parameterAWSSessionToken)
                .toString());
    }

    return awsIam::AWSCredentials(
        _saslClientSession->getParameter(SaslClientSession::parameterUser).toString(),
        _saslClientSession->getParameter(SaslClientSession::parameterPassword).toString());
}

awsIam::AWSCredentials SaslAWSClientConversation::_getLocalAWSCredentials() const {
    // Check the environment variables
    // These are set by AWS Lambda to pass in credentials and can be set by users.
    StringData awsAccessKeyId = getenv("AWS_ACCESS_KEY_ID");
    StringData awsSecretAccessKey = getenv("AWS_SECRET_ACCESS_KEY");
    StringData awsSessionToken = getenv("AWS_SESSION_TOKEN");

    if (!awsAccessKeyId.empty() && !awsSecretAccessKey.empty()) {
        if (!awsSessionToken.empty()) {
            return awsIam::AWSCredentials(awsAccessKeyId.toString(),
                                          awsSecretAccessKey.toString(),
                                          awsSessionToken.toString());
        }

        return awsIam::AWSCredentials(awsAccessKeyId.toString(), awsSecretAccessKey.toString());
    }

    StringData ecsMetadata = getenv("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI");
    if (!ecsMetadata.empty()) {
        return _getEcsCredentials(ecsMetadata);
    }

    return _getEc2Credentials();
}


awsIam::AWSCredentials SaslAWSClientConversation::_getEc2Credentials() const {
    try {

        std::unique_ptr<HttpClient> httpClient = HttpClient::create();

        // The local web server is just a normal HTTP server
        httpClient->allowInsecureHTTP(true);

        // Get the token for authenticating with Instance Metadata Version 2
        // Set a lifetime of 30 seconds since we are only going to use this token for one set of
        // requests.
        std::vector<std::string> headers{"X-aws-ec2-metadata-token-ttl-seconds: 30", "Expect:"};
        httpClient->setHeaders(headers);
        DataBuilder getToken = httpClient->put(getDefaultEC2Host() + "/latest/api/token",
                                               ConstDataRange(nullptr, nullptr));

        StringData token = toString(getToken);

        headers.clear();
        headers.push_back("X-aws-ec2-metadata-token: " + token);
        httpClient->setHeaders(headers);

        // Retrieve the role attached to the EC2 instance
        DataBuilder getRoleResult =
            httpClient->get(getDefaultEC2Host() + "/latest/meta-data/iam/security-credentials/");

        StringData getRoleOutput = toString(getRoleResult);

        std::string role = awsIam::parseRoleFromEC2IamSecurityCredentials(getRoleOutput);

        // Retrieve the temporary credentials of the EC2 instance
        DataBuilder getRoleCredentialsResult = httpClient->get(
            str::stream() << getDefaultEC2Host() + "/latest/meta-data/iam/security-credentials/"
                          << role);

        StringData getRoleCredentialsOutput = toString(getRoleCredentialsResult);

        return awsIam::parseCredentialsFromEC2IamSecurityCredentials(getRoleCredentialsOutput);
    } catch (const DBException& e) {
        // Wrap exceptions from HTTP to make them clearer
        uassertStatusOKWithContext(
            e.toStatus(),
            "Failed to retrieve EC2 Instance Metadata Service Credentials. Ensure there is a role "
            "via an instance profile assigned to this machine.");
    }

    MONGO_UNREACHABLE;
}

awsIam::AWSCredentials SaslAWSClientConversation::_getEcsCredentials(StringData relativeUri) const {
    try {

        std::unique_ptr<HttpClient> httpClient = HttpClient::create();

        // The local web server is just a normal HTTP server
        httpClient->allowInsecureHTTP(true);

        // Retrieve the security token attached to the ECS task
        DataBuilder getRoleResult = httpClient->get(getDefaultECSHost() + relativeUri);

        StringData getRoleOutput = toString(getRoleResult);

        return awsIam::parseCredentialsFromECSTaskIamCredentials(getRoleOutput);
    } catch (const DBException& e) {
        // Wrap exceptions from HTTP to make them clearer
        uassertStatusOKWithContext(e.toStatus(),
                                   "Failed to retrieve ECS Tasks Metadata Credentials. Ensure "
                                   "there is an execution role assigned to this ECS task.");
    }

    MONGO_UNREACHABLE;
}

StatusWith<bool> SaslAWSClientConversation::step(StringData inputData, std::string* outputData) {
    if (_step > 2) {
        return Status(ErrorCodes::AuthenticationFailed,
                      str::stream() << "Invalid AWS authentication step: " << _step);
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

StatusWith<bool> SaslAWSClientConversation::_firstStep(std::string* outputData) {

    *outputData = awsIam::generateClientFirst(&_clientNonce);

    return false;
}

StatusWith<bool> SaslAWSClientConversation::_secondStep(StringData inputData,
                                                        std::string* outputData) {
    auto credentials = _getCredentials();

    *outputData = awsIam::generateClientSecond(inputData, _clientNonce, credentials);

    return true;
}

}  // namespace mongo
