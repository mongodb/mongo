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

    const std::string trimmed = trim(value);
    if (trimmed.empty()) {
        out->inheritProcessDefault = true;
        return Status::OK();
    }

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

    // Comma-separated list of compressor names. Empty segments (e.g. from a trailing comma such as
    // "snappy,") are tolerated and ignored; duplicates are deduplicated while preserving first-seen
    // order (client-advertised order is the negotiation preference order, so keeping the
    // first occurrence gives the operator predictable ordering). Note: a truly empty value ("") was
    // already handled above as "inherit"; here 'trimmed' is non-empty, so an input consisting solely
    // of separators (e.g. "," or ",,") is a malformed list and is rejected below rather than being
    // silently downgraded to inherit, which would hide a configuration typo.
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
        // 'trimmed' was non-empty (the empty "" case is handled earlier as inherit) yet produced no
        // names, i.e. the value was nothing but separators/whitespace (e.g. "," or ", ,"). Reject it
        // as malformed rather than silently treating it as inherit, so an operator's typo surfaces
        // immediately instead of quietly leaving replication compression at the process default.
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
    // synchronized_value<std::string> exposes a locked accessor via operator*; take a snapshot
    // as a plain std::string so we don't hold the lock while parsing.
    std::string snapshot = *gReplicationNetworkCompression;
    // The value can only be set at startup (set_at: [startup]) and is checked by
    // validateReplicationNetworkCompression before it is stored, so by the time replication reads
    // it here it MUST parse cleanly. A parse failure therefore means the invariant was violated
    // (e.g. a future code path wrote gReplicationNetworkCompression directly, bypassing the
    // validator). Fail loudly with the offending value rather than silently degrading to "inherit
    // process default", which would mask the bug and could change compression behavior invisibly.
    auto status = parseReplicationNetworkCompression(snapshot, &result);
    tassert(10130420,
            str::stream() << "stored replicationNetworkCompression value did not parse: '"
                          << snapshot << "': " << status.reason(),
            status.isOK());
    return result;
}

void applyReplicationNetworkCompressionToManager(MessageCompressorManager& mgr) {
    auto setting = getReplicationNetworkCompressionSetting();
    // SERVER-130410: mark this connection as a replication client so the server routes it (and only
    // it) through the replication candidate set. This must be set in every branch below, including
    // "inherit", so heartbeats / shard RPC (which never call this helper) stay on the net set.
    mgr.markReplicationClientForThisSession(true);
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
