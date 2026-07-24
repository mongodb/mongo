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
    // Use mongo::ctype (locale-independent, POSIX/C semantics) rather than <cctype> std::isspace,
    // which is locale-dependent and has UB on negative char values.
    while (b < e && ctype::isSpace(s[b]))
        ++b;
    while (e > b && ctype::isSpace(s[e - 1]))
        --e;
    return std::string(s.substr(b, e - b));
}

std::string toLowerCopy(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    // Use mongo::ctype::toLower (locale-independent) rather than <cctype> std::tolower so the
    // "disabled" token comparison is stable regardless of the process locale.
    for (char c : s) {
        out.push_back(ctype::toLower(c));
    }
    return out;
}

}  // namespace

Status parseReplicationNetworkCompression(std::string_view value,
                                          ReplicationNetworkCompressionSetting* out) {
    *out = ReplicationNetworkCompressionSetting{};

    // Only a truly empty value inherits the process default. A non-empty value that is only
    // whitespace/separators is an explicit (mis)configuration and must fail below rather than be
    // silently treated as inherit, matching the "does not contain any compressor names" rejection.
    if (value.empty()) {
        out->inheritProcessDefault = true;
        return Status::OK();
    }

    const std::string trimmed = trim(value);

    // Any non-empty value is an explicit override of the process default. The struct defaults to
    // inherit=true, so clear it before returning from either the disabled or allow-list branches.
    out->inheritProcessDefault = false;

    // "disabled" (case-insensitive) is a reserved token. It cannot be combined with algorithm
    // names: e.g. "disabled,snappy" is rejected to avoid ambiguity.
    const std::string lowered = toLowerCopy(trimmed);
    if (lowered == "disabled") {
        out->disabled = true;
        return Status::OK();
    }

    // Parse comma-separated compressor names. Empty segments are ignored, duplicates are removed
    // while preserving first-seen order, and a non-empty value with no names is rejected.
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
        // Reject non-empty values that contained only separators/whitespace instead of treating
        // them as inherit, so configuration typos surface at startup.
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
    // Take a plain std::string snapshot so we don't hold the synchronized_value lock while parsing.
    std::string snapshot = *gReplicationNetworkCompression;
    // Startup validation should guarantee this parses; fail loudly if that invariant is broken.
    auto status = parseReplicationNetworkCompression(snapshot, &result);
    tassert(10130420,
            str::stream() << "stored replicationNetworkCompression value did not parse: '"
                          << snapshot << "': " << status.reason(),
            status.isOK());
    return result;
}

void applyReplicationNetworkCompressionToManager(MessageCompressorManager& mgr) {
    auto setting = getReplicationNetworkCompressionSetting();
    // Mark replication data-plane connections so hello carries the replication marker and compressed
    // bytes are counted under serverStatus().repl.compression. Other internal connections do not
    // call this helper and continue using the normal net.compression policy.
    mgr.markReplicationClientForThisSession(true);
    mgr.countAsReplicationCompressionTrafficForThisSession(true);
    if (setting.inheritProcessDefault) {
        // Advertise the process-wide net.compression.compressors list: clear any per-session
        // suppression and allow-list left over from a previous (re)connect.
        mgr.enableCompressionForThisSession();
        mgr.setCompressorAllowListForThisSession({});
    } else if (setting.disabled) {
        // Negotiate this connection uncompressed regardless of net.compression.compressors.
        mgr.disableCompressionForThisSession();
        mgr.setCompressorAllowListForThisSession({});
    } else {
        // Advertise the operator-chosen subset; clientBegin() intersects it with the process-wide
        // capability set before advertising.
        mgr.enableCompressionForThisSession();
        mgr.setCompressorAllowListForThisSession(std::move(setting.allowList));
    }
}

}  // namespace repl
}  // namespace mongo
