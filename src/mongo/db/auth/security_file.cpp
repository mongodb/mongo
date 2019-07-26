/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/auth/security_key.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/util/str.h"

#include "yaml-cpp/yaml.h"

namespace mongo {
namespace {
std::string stripString(const std::string& filename, const std::string& str) {
    std::string out;
    out.reserve(str.size());

    std::copy_if(str.begin(), str.end(), std::back_inserter(out), [&](const char ch) {
        // don't copy any whitespace
        if ((ch >= '\x09' && ch <= '\x0D') || ch == ' ') {
            return false;
            // uassert if string contains any non-base64 characters
        } else if ((ch < 'A' || ch > 'Z') && (ch < 'a' || ch > 'z') && (ch < '0' || ch > '9') &&
                   ch != '+' && ch != '/' && ch != '=') {
            uasserted(ErrorCodes::UnsupportedFormat,
                      str::stream() << "invalid char in key file " << filename << ": " << str);
        }
        return true;
    });

    return out;
}

}  // namespace

StatusWith<std::vector<std::string>> readSecurityFile(const std::string& filename) try {
    struct stat stats;

    // check obvious file errors
    if (stat(filename.c_str(), &stats) == -1) {
        return Status(ErrorCodes::InvalidPath,
                      str::stream()
                          << "Error reading file " << filename << ": " << strerror(errno));
    }

#if !defined(_WIN32)
    // check permissions: must be X00, where X is >= 4
    if ((stats.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return Status(ErrorCodes::InvalidPath,
                      str::stream() << "permissions on " << filename << " are too open");
    }
#endif

    std::vector<std::string> ret;
    const std::function<void(const YAML::Node&)> visitNode = [&](const YAML::Node& node) {
        if (node.IsScalar()) {
            ret.push_back(stripString(filename, node.Scalar()));
        } else if (node.IsSequence()) {
            for (const auto& child : node) {
                visitNode(child);
            }
        } else {
            uasserted(ErrorCodes::UnsupportedFormat,
                      "Only strings and sequences are supported for YAML key files");
        }
    };

    visitNode(YAML::LoadFile(filename));

    uassert(50981,
            str::stream() << "Security key file " << filename << " does not contain any valid keys",
            !ret.empty());

    return ret;
} catch (const YAML::BadFile& e) {
    return Status(ErrorCodes::InvalidPath,
                  str::stream() << "error opening file: " << filename << ": " << e.what());
} catch (const YAML::ParserException& e) {
    return Status(ErrorCodes::UnsupportedFormat,
                  str::stream() << "error reading file: " << filename << ": " << e.what());
} catch (const DBException& e) {
    return e.toStatus();
}

}  // namespace mongo
