// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <string_view>
#include <type_traits>

#include <grpcpp/security/server_credentials.h>
#include <grpcpp/support/status_code_enum.h>

namespace mongo::transport::grpc::util {
namespace constants {
using namespace std::literals::string_view_literals;
/**
 * Wire version constant corresponding to the first wire version that supports using gRPC.
 */
static constexpr auto kMinimumWireVersion = 26;

static constexpr auto kAuthenticatedCommandStreamMethodName =
    "/mongodb.CommandService/AuthenticatedCommandStream";
static constexpr auto kUnauthenticatedCommandStreamMethodName =
    "/mongodb.CommandService/UnauthenticatedCommandStream";

// Server-provided metadata keys.
// This is defined as a std::string instead of std::string_view to avoid having to copy it when
// passing to gRPC APIs that expect a const std::string&.
extern const std::string kClusterMaxWireVersionKey;

// Client-provided metadata keys.
static constexpr std::string_view kAuthenticationTokenKey = "authorization"sv;
static constexpr std::string_view kClientIdKey = "mongodb-clientid"sv;
static constexpr std::string_view kClientMetadataKey = "mongodb-client"sv;
static constexpr std::string_view kWireVersionKey = "mongodb-wireversion"sv;
}  // namespace constants

/**
 * Parse a PEM-encoded file that contains a single certificate and its associated private key
 * into a PemKeyCertPair.
 */
::grpc::SslServerCredentialsOptions::PemKeyCertPair parsePEMKeyFile(std::string_view filePath);

/**
 * Converts a Mongo URI into a gRPC formatted string.
 */
std::string toGRPCFormattedURI(const HostAndPort& address);

// See: https://grpc.github.io/grpc/cpp/md_doc_naming.html
inline bool isUnixSchemeGRPCFormattedURI(std::string_view uri) {
    return uri.starts_with("unix:");
}

/**
 * Parses a gRPC-formatted URI to a HostAndPort, throwing an exception on failure.
 * See: https://grpc.github.io/grpc/cpp/md_doc_naming.html
 */
HostAndPort parseGRPCFormattedURI(std::string_view uri);

/**
 * Converts a gRPC status code into its corresponding MongoDB error code.
 */
[[MONGO_MOD_PUBLIC]] ErrorCodes::Error statusToErrorCode(::grpc::StatusCode statusCode);

/**
 * Converts a MongoDB error code into its corresponding gRPC status code.
 * Note that the mapping between gRPC status codes and MongoDB errors codes is not 1 to 1, so the
 * following does not have to evaluate to true:
 * `errorToStatusCode(statusToErrorCode(sc)) == sc`
 */
::grpc::StatusCode errorToStatusCode(ErrorCodes::Error errorCode);

/**
 * Converts a MongoDB status to its gRPC counterpart, and vice versa.
 * Prefer using this over direct invocations of `errorToStatusCode` and `statusToErrorCode`.
 */
template <typename StatusType>
inline auto convertStatus(StatusType status) {
    if constexpr (std::is_same<StatusType, Status>::value) {
        return ::grpc::Status(errorToStatusCode(status.code()), status.reason());
    } else {
        static_assert(std::is_same<StatusType, ::grpc::Status>::value == true);
        return Status(statusToErrorCode(status.error_code()), status.error_message());
    }
}

}  // namespace mongo::transport::grpc::util
