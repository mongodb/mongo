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

#include "mongo/client/sasl_aws_client_protocol.h"

#include "mongo/base/data_range.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/client/sasl_aws_client_protocol_gen.h"
#include "mongo/client/sasl_aws_protocol_common_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/kms_message_support.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <kms_message/kms_caller_identity_request.h>
#include <kms_message/kms_message_defines.h>
#include <kms_message/kms_request.h>

namespace mongo {
namespace awsIam {
namespace {
// Secure Random for AWS SASL Nonce generation
stdx::mutex saslAWSClientMutex;
SecureRandom saslAWSClientGen;

std::vector<char> generateClientNonce() {

    std::vector<char> ret;
    ret.resize(kClientFirstNonceLength);

    {
        stdx::lock_guard<stdx::mutex> lk(saslAWSClientMutex);
        saslAWSClientGen.fill(ret.data(), ret.size());
    }

    return ret;
}

/**
 * Returns false if a dns name contains an empty part.
 * Good: a.b  or a.b.c or a
 * Bad: a..b or a.b..c
 */
bool validateHostNameParts(StringData str) {
    size_t pos = str.find('.');
    if (pos != std::string::npos) {
        while (true) {
            size_t last_pos = pos;
            pos = str.find('.', last_pos + 1);
            if (pos == std::string::npos) {
                break;
            }
            if (last_pos + 1 == pos) {
                return false;
            }
        }
    }

    return true;
}

void uassertKmsRequestInternal(kms_request_t* request, const char* file, int line, bool ok) {
    if (!ok) {
        const char* msg = kms_request_get_error(request);
        uasserted(51299,
                  str::stream() << "Internal AWS IAM Error: " << msg << " at " << file << ":"
                                << line);
    }
}

template <typename T>
AWSCredentials parseCredentials(StringData data) {
    BSONObj obj = fromjson(std::string{data});

    auto creds = T::parse(obj, IDLParserContext("security-credentials"));

    return AWSCredentials(std::string{creds.getAccessKeyId()},
                          std::string{creds.getSecretAccessKey()},
                          std::string{creds.getToken()});
}
}  // namespace


std::string generateClientFirst(std::vector<char>* clientNonce) {
    *clientNonce = generateClientNonce();

    AwsClientFirst first;
    first.setNonce(*clientNonce);
    first.setGs2_cb_flag(static_cast<int>('n'));

    return convertToByteString(first);
}

#define uassertKmsRequest(X) uassertKmsRequestInternal(request.get(), __FILE__, __LINE__, (X));

std::string generateClientSecond(StringData serverFirstBase64,
                                 const std::vector<char>& clientNonce,
                                 const AWSCredentials& credentials) {
    dassert(clientNonce.size() == kClientFirstNonceLength);
    auto serverFirst = convertFromByteString<AwsServerFirst>(serverFirstBase64);

    uassert(51298,
            "Nonce must be 64 bytes",
            serverFirst.getServerNonce().length() == kServerFirstNonceLength);

    uassert(51297,
            "First part of nonce must match client",
            std::equal(serverFirst.getServerNonce().data(),
                       serverFirst.getServerNonce().data() + kClientFirstNonceLength,
                       clientNonce.begin(),
                       clientNonce.end()) == true);

    uassert(51296,
            "Host name length is incorrect",
            !serverFirst.getStsHost().empty() &&
                serverFirst.getStsHost().size() < kMaxStsHostNameLength);

    uassert(51295,
            "Host name is not allowed to have a empty DNS name part.",
            validateHostNameParts(serverFirst.getStsHost()));


    auto request = UniqueKmsRequest(kms_caller_identity_request_new(NULL));

    // Use current time
    uassertKmsRequest(kms_request_set_date(request.get(), nullptr));

    // Region is derived from host
    uassertKmsRequest(
        kms_request_set_region(request.get(), getRegionFromHost(serverFirst.getStsHost()).c_str()));

    // sts is always the name of the service
    uassertKmsRequest(kms_request_set_service(request.get(), kAwsServiceName.data()));

    uassertKmsRequest(kms_request_add_header_field(
        request.get(), "Host", std::string{serverFirst.getStsHost()}.c_str()));

    auto serverNonce = serverFirst.getServerNonce();
    uassertKmsRequest(kms_request_add_header_field(
        request.get(),
        kMongoServerNonceHeader.data(),
        base64::encode(StringData(serverNonce.data(), serverNonce.length())).c_str()));

    uassertKmsRequest(kms_request_add_header_field(
        request.get(), kMongoGS2CBHeader.data(), kMongoDefaultGS2CBFlag.data()));

    uassertKmsRequest(
        kms_request_set_access_key_id(request.get(), credentials.accessKeyId.c_str()));
    uassertKmsRequest(
        kms_request_set_secret_key(request.get(), credentials.secretAccessKey.c_str()));


    AwsClientSecond second;

    if (credentials.sessionToken) {
        // TODO: move this into kms-message
        uassertKmsRequest(kms_request_add_header_field(
            request.get(), "X-Amz-Security-Token", credentials.sessionToken.value().c_str()));

        second.setXAmzSecurityToken(credentials.sessionToken.value());
    }

    UniqueKmsCharBuffer kmsSignature(kms_request_get_signature(request.get()));
    second.setAuthHeader(kmsSignature.get());

    second.setXAmzDate(kms_request_get_canonical_header(request.get(), kXAmzDateHeader.data()));

    return convertToByteString(second);
}

std::string getRegionFromHost(StringData host) {
    if (host == kAwsDefaultStsHost) {
        return std::string{kAwsDefaultRegion};
    }

    size_t firstPeriod = host.find('.');
    if (firstPeriod == std::string::npos) {
        return std::string{kAwsDefaultRegion};
    }

    size_t secondPeriod = host.find('.', firstPeriod + 1);
    if (secondPeriod == std::string::npos) {
        return std::string{host.substr(firstPeriod + 1)};
    }

    return std::string{host.substr(firstPeriod + 1, secondPeriod - firstPeriod - 1)};
}

std::string parseRoleFromEC2IamSecurityCredentials(StringData data) {
    // Before the Nov 2019 AWS update, they added \n to the role_name.
    size_t pos = data.find('\n');

    if (pos == std::string::npos) {
        pos = data.size();
    }

    return std::string{data.substr(0, pos)};
}

AWSCredentials parseCredentialsFromEC2IamSecurityCredentials(StringData data) {
    return parseCredentials<Ec2SecurityCredentials>(data);
}

AWSCredentials parseCredentialsFromECSTaskIamCredentials(StringData data) {
    return parseCredentials<EcsTaskSecurityCredentials>(data);
}

MONGO_INITIALIZER(SASLRegisterKMS)(::mongo::InitializerContext*) {
    kms_message_init();
}

}  // namespace awsIam
}  // namespace mongo
