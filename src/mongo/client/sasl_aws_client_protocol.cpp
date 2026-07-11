// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/kms_message_support.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <kms_message/kms_caller_identity_request.h>
#include <kms_message/kms_message_defines.h>
#include <kms_message/kms_request.h>

namespace mongo {
namespace awsIam {
namespace {
// Secure Random for AWS SASL Nonce generation
std::mutex saslAWSClientMutex;
SecureRandom saslAWSClientGen;

std::vector<char> generateClientNonce() {

    std::vector<char> ret;
    ret.resize(kClientFirstNonceLength);

    {
        std::lock_guard<std::mutex> lk(saslAWSClientMutex);
        saslAWSClientGen.fill(ret.data(), ret.size());
    }

    return ret;
}

/**
 * Returns false if a dns name contains an empty part.
 * Good: a.b  or a.b.c or a
 * Bad: a..b or a.b..c
 */
bool validateHostNameParts(std::string_view str) {
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
AWSCredentials parseCredentials(std::string_view data) {
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

std::string generateClientSecond(std::string_view serverFirstBase64,
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
    uassertKmsRequest(kms_request_set_service(request.get(), std::string{kAwsServiceName}.c_str()));

    uassertKmsRequest(kms_request_add_header_field(
        request.get(), "Host", std::string{serverFirst.getStsHost()}.c_str()));

    auto serverNonce = serverFirst.getServerNonce();
    uassertKmsRequest(kms_request_add_header_field(
        request.get(),
        std::string{kMongoServerNonceHeader}.c_str(),
        base64::encode(std::string_view(serverNonce.data(), serverNonce.length())).c_str()));

    uassertKmsRequest(kms_request_add_header_field(request.get(),
                                                   std::string{kMongoGS2CBHeader}.c_str(),
                                                   std::string{kMongoDefaultGS2CBFlag}.c_str()));

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

    second.setXAmzDate(
        kms_request_get_canonical_header(request.get(), std::string{kXAmzDateHeader}.c_str()));

    return convertToByteString(second);
}

std::string getRegionFromHost(std::string_view host) {
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

std::string parseRoleFromEC2IamSecurityCredentials(std::string_view data) {
    // Before the Nov 2019 AWS update, they added \n to the role_name.
    size_t pos = data.find('\n');

    if (pos == std::string::npos) {
        pos = data.size();
    }

    return std::string{data.substr(0, pos)};
}

AWSCredentials parseCredentialsFromEC2IamSecurityCredentials(std::string_view data) {
    return parseCredentials<Ec2SecurityCredentials>(data);
}

AWSCredentials parseCredentialsFromECSTaskIamCredentials(std::string_view data) {
    return parseCredentials<EcsTaskSecurityCredentials>(data);
}

MONGO_INITIALIZER(SASLRegisterKMS)(::mongo::InitializerContext*) {
    kms_message_init();
}

}  // namespace awsIam
}  // namespace mongo
