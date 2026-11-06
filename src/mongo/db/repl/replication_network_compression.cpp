/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/repl/replication_network_compression.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/str.h"

#include <algorithm>

namespace mongo {
namespace repl {
namespace {

std::string trim(std::string_view s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && ctype::isSpace(s[b]))
        ++b;
    while (e > b && ctype::isSpace(s[e - 1]))
        --e;
    return std::string(s.substr(b, e - b));
}

std::string toLowerCopy(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(ctype::toLower(c));
    }
    return out;
}

}  // namespace

Status parseReplicationNetworkCompression(std::string_view value,
                                          ReplicationNetworkCompressionSetting* out) {
    *out = ReplicationNetworkCompressionSetting{};

    // Only "" inherits. A value of just spaces or commas is rejected below.
    if (value.empty()) {
        out->inheritProcessDefault = true;
        return Status::OK();
    }

    const std::string trimmed = trim(value);
    out->inheritProcessDefault = false;

    const std::string lowered = toLowerCopy(trimmed);
    if (lowered == "disabled") {
        out->disabled = true;
        return Status::OK();
    }

    // Skip empty entries and drop duplicates, keeping the first one.
    std::vector<std::string> parsed;
    size_t start = 0;
    while (start <= trimmed.size()) {
        size_t comma = trimmed.find(',', start);
        std::string_view tok = (comma == std::string::npos)
            ? std::string_view(trimmed).substr(start)
            : std::string_view(trimmed).substr(start, comma - start);
        std::string name = trim(tok);
        if (!name.empty()) {
            if (toLowerCopy(name) == "disabled") {
                return {ErrorCodes::BadValue,
                        "replicationNetworkCompression: 'disabled' cannot be combined with "
                        "compressor names"};
            }
            if (std::find(parsed.begin(), parsed.end(), name) == parsed.end()) {
                parsed.push_back(std::move(name));
            }
        }
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }

    if (parsed.empty()) {
        return {ErrorCodes::BadValue,
                str::stream() << "replicationNetworkCompression: '" << value
                              << "' does not contain any compressor names"};
    }

    out->allowList = std::move(parsed);
    return Status::OK();
}

Status validateReplicationNetworkCompression(std::string_view value,
                                             const boost::optional<TenantId>& /*tenantId*/) {
    ReplicationNetworkCompressionSetting sink;
    return parseReplicationNetworkCompression(value, &sink);
}

ReplicationNetworkCompressionSetting getReplicationNetworkCompressionSetting() {
    ReplicationNetworkCompressionSetting result;
    std::string snapshot = *gReplicationNetworkCompression;
    auto status = parseReplicationNetworkCompression(snapshot, &result);
    tassert(10130420,
            str::stream() << "stored replicationNetworkCompression value did not parse: '"
                          << snapshot << "': " << status.reason(),
            status.isOK());
    return result;
}

void applyReplicationNetworkCompressionToManager(MessageCompressorManager& mgr) {
    auto setting = getReplicationNetworkCompressionSetting();
    mgr.markReplicationClientForThisSession(true);
    mgr.countAsReplicationCompressionTrafficForThisSession(true);
    if (setting.inheritProcessDefault) {
        mgr.enableCompressionForThisSession();
        mgr.setCompressorAllowListForThisSession({});
    } else if (setting.disabled) {
        mgr.disableCompressionForThisSession();
        mgr.setCompressorAllowListForThisSession({});
    } else {
        mgr.enableCompressionForThisSession();
        mgr.setCompressorAllowListForThisSession(std::move(setting.allowList));
    }
}

void negotiateHelloCompression(
    MessageCompressorManager& mgr,
    bool isReplicationClient,
    const boost::optional<std::vector<std::string_view>>& clientCompressors,
    BSONObjBuilder* result) {
    boost::optional<std::vector<std::string>> allowList;
    if (isReplicationClient) {
        auto setting = getReplicationNetworkCompressionSetting();
        if (setting.disabled) {
            allowList.emplace();
        } else if (!setting.inheritProcessDefault) {
            allowList.emplace(std::move(setting.allowList));
        }
        // Only the first hello carries the field, so never clear the flag.
        mgr.countAsReplicationCompressionTrafficForThisSession(true);
    }
    mgr.serverNegotiate(clientCompressors, result, allowList);
}

}  // namespace repl
}  // namespace mongo
