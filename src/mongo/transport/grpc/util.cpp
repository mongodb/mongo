// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/grpc/util.h"

#include "mongo/client/mongo_uri.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_util.h"
#include "mongo/util/testing_proctor.h"

#include <string_view>

#include <fmt/format.h>

namespace mongo::transport::grpc::util {
namespace constants {
using namespace std::literals::string_view_literals;
const std::string kClusterMaxWireVersionKey = "mongodb-maxwireversion";
constexpr std::string_view kUriSchemes[] = {"dns:"sv, "ipv4:"sv, "ipv6:"sv, "unix:"sv};
}  // namespace constants

::grpc::SslServerCredentialsOptions::PemKeyCertPair parsePEMKeyFile(std::string_view filePath) {

    ::grpc::SslServerCredentialsOptions::PemKeyCertPair certPair;

    auto certificateKeyFileContents = uassertStatusOK(ssl_util::readPEMFile(filePath));
    certPair.cert_chain = certificateKeyFileContents;
    certPair.private_key = certificateKeyFileContents;

    return certPair;
}

std::string toGRPCFormattedURI(const HostAndPort& address) {
    if (isUnixDomainSocket(address.host())) {
        return fmt::format("unix://{}", address.host());
    } else {
        return address.toString();
    }
}

HostAndPort parseGRPCFormattedURI(std::string_view uri) {
    // See: https://github.com/grpc/grpc/issues/35006
    if (uri == "unix:") {
        return HostAndPort("", 0);
    }

    // gRPC URIs can be prefixed with a scheme (e.g. "unix:///blah.sock"). If this URI contains a
    // scheme, find the end of it and begin parsing from that point onward.
    for (auto scheme : constants::kUriSchemes) {
        if (uri.starts_with(scheme)) {
            uri.remove_prefix(scheme.size());
            break;
        }
    }

    return HostAndPort::parseThrowing(uassertStatusOK(uriDecode(uri)));
}

ErrorCodes::Error statusToErrorCode(::grpc::StatusCode statusCode) {
    switch (statusCode) {
        case ::grpc::OK:
            return ErrorCodes::OK;
        case ::grpc::UNAUTHENTICATED:
            return ErrorCodes::AuthenticationFailed;
        case ::grpc::CANCELLED:
            return ErrorCodes::CallbackCanceled;
        case ::grpc::INVALID_ARGUMENT:
            return ErrorCodes::BadValue;
        case ::grpc::DEADLINE_EXCEEDED:
            return ErrorCodes::ExceededTimeLimit;
        case ::grpc::FAILED_PRECONDITION:
            return ErrorCodes::RPCProtocolNegotiationFailed;
        case ::grpc::UNIMPLEMENTED:
            return ErrorCodes::NotImplemented;
        case ::grpc::INTERNAL:
            return ErrorCodes::InternalError;
        case ::grpc::UNAVAILABLE:
            return ErrorCodes::HostUnreachable;
        case ::grpc::PERMISSION_DENIED:
            return ErrorCodes::Unauthorized;
        case ::grpc::RESOURCE_EXHAUSTED:
            return ErrorCodes::ResourceExhausted;
        default:
            return ErrorCodes::UnknownError;
    }
}

::grpc::StatusCode errorToStatusCode(ErrorCodes::Error errorCode) {
    switch (errorCode) {
        case ErrorCodes::OK:
            return ::grpc::OK;
        case ErrorCodes::UnknownError:
            return ::grpc::UNKNOWN;
        case ErrorCodes::InterruptedAtShutdown:
        case ErrorCodes::ShutdownInProgress:
            return ::grpc::UNAVAILABLE;
        case ErrorCodes::CallbackCanceled:
        case ErrorCodes::ClientMarkedKilled:
            return ::grpc::CANCELLED;
        default:
            invariant(TestingProctor::instance().isEnabled(),
                      fmt::format("No known conversion for MongoDB error code: ",
                                  fmt::underlying(errorCode)));
            return ::grpc::UNKNOWN;
    }
}

}  // namespace mongo::transport::grpc::util
