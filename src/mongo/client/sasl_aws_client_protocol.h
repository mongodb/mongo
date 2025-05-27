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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/client/sasl_aws_protocol_common.h"

#include <string>
#include <vector>

namespace mongo {
namespace awsIam {

/**
 * Generate client first message for AWS Auth.
 *
 * Returns nonce as out parameter for client to store
 */
std::string generateClientFirst(std::vector<char>* clientNonce);

/**
 * Parse AWS Auth server first message and generate client second.
 */
std::string generateClientSecond(StringData serverFirst,
                                 const std::vector<char>& clientNonce,
                                 const AWSCredentials& credentials);

/**
 * Get the AWS Role Name from a request to
 * http://169.254.169.254/latest/meta-data/iam/security-credentials/
 *
 * The input is expected to be a simple line that ends in a newline (\n).
 */
std::string parseRoleFromEC2IamSecurityCredentials(StringData data);

/**
 * Get the AWS region from a DNS Name
 *
 * Region by default is "us-east-1" since this is the implicit region for "sts.amazonaws.com".
 *
 * Host                   Region
 * sts.amazonaws.com      us-east-1
 * first.second.third     second
 * first.second           second
 * first                  us-east-1
 */
std::string getRegionFromHost(StringData host);

/**
 * Get a set of AWS Credentials from a request to
 *  http://169.254.169.254/latest/meta-data/iam/security-credentials/<ROLE_NAME>
 *
 * where ROLE_NAME comes from parseRoleFromEC2IamSecurityCredentials.
 *
 * Per
 * https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/iam-roles-for-amazon-ec2.html#instance-metadata-security-credentials,
 * we expect the following JSON:
 *
 * {
 *   "Code" : "Success",
 *   "LastUpdated" : "DATE",
 *   "Type" : "AWS-HMAC",
 *   "AccessKeyId" : "ACCESS_KEY_ID",
 *   "SecretAccessKey" : "SECRET_ACCESS_KEY",
 *   "Token" : "SECURITY_TOKEN_STRING",
 *   "Expiration" : "EXPIRATION_DATE"
 * }
 */
AWSCredentials parseCredentialsFromEC2IamSecurityCredentials(StringData data);

/**
 * Get a set of AWS Credentials from a request to
 *  http://169.254.170.2$AWS_CONTAINER_CREDENTIALS_RELATIVE_URI
 *
 * where AWS_CONTAINER_CREDENTIALS_RELATIVE_URI is an environment variable.
 *
 * Per https://docs.aws.amazon.com/AmazonECS/latest/developerguide/task-iam-roles.html,
 * we expect the following JSON:
 *
 * {
 *     "AccessKeyId": "ACCESS_KEY_ID",
 *     "Expiration": "EXPIRATION_DATE",
 *     "RoleArn": "TASK_ROLE_ARN",
 *     "SecretAccessKey": "SECRET_ACCESS_KEY",
 *     "Token": "SECURITY_TOKEN_STRING"
 * }
 */
AWSCredentials parseCredentialsFromECSTaskIamCredentials(StringData data);

}  // namespace awsIam
}  // namespace mongo
