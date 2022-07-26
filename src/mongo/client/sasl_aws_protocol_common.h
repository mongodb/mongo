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

#include <boost/optional.hpp>
#include <string>
#include <vector>

#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_validated.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/client/sasl_aws_protocol_common_gen.h"
#include "mongo/rpc/object_check.h"

namespace mongo {

namespace awsIam {
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

static constexpr auto kMongoServerNonceHeader = "x-mongodb-server-nonce"_sd;
static constexpr auto kMongoGS2CBHeader = "x-mongodb-gs2-cb-flag"_sd;
static constexpr auto kChannelBindingDataHeader = "x-mongodb-channel-binding-data"_sd;
static constexpr auto kChannelBindingTypePrefixHeader = "x-mongodb-channel-type-prefix"_sd;

static constexpr auto kMongoDefaultGS2CBFlag = "n"_sd;

static constexpr auto kAwsServiceName = "sts"_sd;

static constexpr auto kAwsDefaultRegion = "us-east-1"_sd;
static constexpr auto kAwsDefaultStsHost = "sts.amazonaws.com"_sd;

static constexpr auto kXAmzDateHeader = "X-Amz-Date"_sd;

/**
 * Deserialize a std::string to an IDL object.
 */
template <typename T>
T convertFromByteString(StringData rawString) {
    ConstDataRange cdr(rawString.rawData(), rawString.size());

    auto clientFirstBson = cdr.read<Validated<BSONObj>>();

    return T::parse(IDLParserContext("sasl"), clientFirstBson);
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
