// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range_cursor.h"
#include "mongo/base/status_with.h"
#include "mongo/client/sasl_aws_protocol_common_gen.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {

namespace awsIam {
using namespace std::literals::string_view_literals;
/**
 * Common constants shared by AWS client and server code.
 */
/**
 * Client and Server Nonce lengths
 */
static constexpr size_t kClientFirstNonceLength = 32;
static constexpr size_t kServerFirstNoncePieceLength = 32;
static constexpr size_t kServerFirstNonceLength =
    kClientFirstNonceLength + kServerFirstNoncePieceLength;

static constexpr size_t kMaxStsHostNameLength = 255;

static constexpr auto kMongoServerNonceHeader = "x-mongodb-server-nonce"sv;
static constexpr auto kMongoGS2CBHeader = "x-mongodb-gs2-cb-flag"sv;
static constexpr auto kChannelBindingDataHeader = "x-mongodb-channel-binding-data"sv;
static constexpr auto kChannelBindingTypePrefixHeader = "x-mongodb-channel-type-prefix"sv;

static constexpr auto kMongoDefaultGS2CBFlag = "n"sv;

static constexpr auto kAwsServiceName = "sts"sv;

static constexpr auto kAwsDefaultRegion = "us-east-1"sv;
static constexpr auto kAwsDefaultStsHost = "sts.amazonaws.com"sv;

static constexpr auto kXAmzDateHeader = "X-Amz-Date"sv;

/**
 * Deserialize a std::string to an IDL object.
 */
template <typename T>
T convertFromByteString(std::string_view rawString) {
    ConstDataRange cdr(rawString.data(), rawString.size());

    BSONObj clientFirstBson{cdr.read<rpc::ValidatedBSONObj>()};

    return T::parse(clientFirstBson, IDLParserContext("sasl"));
}

/**
 * Convert an IDL object to a std::string
 */
template <typename T>
std::string convertToByteString(T object) {
    BSONObj obj = object.toBSON();

    return std::string((obj.objdata()), obj.objsize());
}

/**
 * AWS Credentials holder
 */
struct AWSCredentials {
    AWSCredentials() = default;

    /**
     * Construct for regular AWS credentials
     */
    explicit AWSCredentials(std::string accessKeyIdParam, std::string secretAccessKeyParam)
        : accessKeyId(std::move(accessKeyIdParam)),
          secretAccessKey(std::move(secretAccessKeyParam)) {}

    /**
     * Construct for temporary AWS credentials
     */
    explicit AWSCredentials(std::string accessKeyIdParam,
                            std::string secretAccessKeyParam,
                            std::string sessionTokenParam)
        : accessKeyId(std::move(accessKeyIdParam)),
          secretAccessKey(std::move(secretAccessKeyParam)),
          sessionToken(std::move(sessionTokenParam)) {}

    // AWS ACCESS_KEY_ID
    std::string accessKeyId;

    // AWS SECRET_ACCESS_KEY
    std::string secretAccessKey;

    // AWS SESSION TOKEN
    // Generated for temporary credentials
    boost::optional<std::string> sessionToken;
};

}  // namespace awsIam

}  // namespace mongo
