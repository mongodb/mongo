// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/net/socket_exception.h"

#include "mongo/util/str.h"

namespace mongo {

namespace {

std::string getStringType(SocketErrorKind kind) {
    switch (kind) {
        case SocketErrorKind::CLOSED:
            return "CLOSED";
        case SocketErrorKind::RECV_ERROR:
            return "RECV_ERROR";
        case SocketErrorKind::SEND_ERROR:
            return "SEND_ERROR";
        case SocketErrorKind::RECV_TIMEOUT:
            return "RECV_TIMEOUT";
        case SocketErrorKind::SEND_TIMEOUT:
            return "SEND_TIMEOUT";
        case SocketErrorKind::FAILED_STATE:
            return "FAILED_STATE";
        case SocketErrorKind::CONNECT_ERROR:
            return "CONNECT_ERROR";
        default:
            return "UNKNOWN";  // should never happen
    }
}

}  // namespace

Status makeSocketError(SocketErrorKind kind, const std::string& server, const std::string& extra) {
    StringBuilder ss;
    ss << "socket exception [" << getStringType(kind) << "]";

    if (!server.empty())
        ss << " server [" << server << "]";

    if (!extra.empty())
        ss << ' ' << extra;

    return Status(ErrorCodes::SocketException, ss.str());
}


}  // namespace mongo
