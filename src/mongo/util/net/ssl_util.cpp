// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/net/ssl_util.h"

#include "mongo/util/str.h"

#include <string_view>

namespace mongo {

namespace ssl_util {

StatusWith<std::string_view> findPEMBlob(std::string_view blob,
                                         std::string_view type,
                                         size_t position,
                                         bool allowEmpty) {
    std::string header = str::stream() << "-----BEGIN " << type << "-----";
    std::string trailer = str::stream() << "-----END " << type << "-----";

    size_t headerPosition = blob.find(header, position);
    if (headerPosition == std::string::npos) {
        if (allowEmpty) {
            return std::string_view();
        } else {
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "Failed to find PEM blob header: " << header);
        }
    }

    size_t trailerPosition = blob.find(trailer, headerPosition);
    if (trailerPosition == std::string::npos) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "Failed to find PEM blob trailer: " << trailer);
    }

    trailerPosition += trailer.size();

    return std::string_view(blob.data() + headerPosition, trailerPosition - headerPosition);
}


StatusWith<std::string> readPEMFile(std::string_view fileName) {
    // Calling `toString()` is necessary as `fileName` does not have to be null-terminated.
    std::ifstream pemFile(std::string{fileName}, std::ios::binary);
    if (!pemFile.is_open()) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      fmt::format("Failed to open PEM file: {}", fileName));
    }

    std::string buf((std::istreambuf_iterator<char>(pemFile)), std::istreambuf_iterator<char>());

    pemFile.close();

    return buf;
}

}  // namespace ssl_util

}  // namespace mongo
