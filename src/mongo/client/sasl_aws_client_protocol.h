// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/sasl_aws_protocol_common.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>
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
std::string generateClientSecond(std::string_view serverFirst,
                                 const std::vector<char>& clientNonce,
                                 const AWSCredentials& credentials);

/**
 * Get the AWS Role Name from a request to
 * http://169.254.169.254/latest/meta-data/iam/security-credentials/
 *
 * The input is expected to be a simple line that ends in a newline (\n).
 */
std::string parseRoleFromEC2IamSecurityCredentials(std::string_view data);

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
std::string getRegionFromHost(std::string_view host);

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
AWSCredentials parseCredentialsFromEC2IamSecurityCredentials(std::string_view data);

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
AWSCredentials parseCredentialsFromECSTaskIamCredentials(std::string_view data);

}  // namespace awsIam
}  // namespace mongo
