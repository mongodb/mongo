/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/util/net/ssl_util.h"
#include "mongo/util/str.h"

namespace mongo {

namespace ssl_util {

StatusWith<StringData> findPEMBlob(StringData blob,
                                   StringData type,
                                   size_t position,
                                   bool allowEmpty) {
    std::string header = str::stream() << "-----BEGIN " << type << "-----";
    std::string trailer = str::stream() << "-----END " << type << "-----";

    size_t headerPosition = blob.find(header, position);
    if (headerPosition == std::string::npos) {
        if (allowEmpty) {
            return StringData();
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

    return StringData(blob.rawData() + headerPosition, trailerPosition - headerPosition);
}


StatusWith<std::string> readPEMFile(StringData fileName) {
    // Calling `toString()` is necessary as `fileName` does not have to be null-terminated.
    std::ifstream pemFile(fileName.toString(), std::ios::binary);
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
