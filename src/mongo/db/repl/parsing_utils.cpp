// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/parsing_utils.h"

#include "mongo/logv2/log.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {
namespace repl {
std::tuple<std::string, std::vector<HostAndPort>> parseReplSetSeedList(
    ReplicationCoordinatorExternalState* externalState, const std::string_view replSetString) {
    uassert(10283305, "bad --replSet command line config string (empty)", !replSetString.empty());

    std::string setName;
    std::vector<HostAndPort> seeds;

    const auto slash = replSetString.find('/');
    if (slash == std::string_view::npos) {
        // replSet name with no seed hosts is valid.
        return {std::string{replSetString}, seeds};
    } else {
        setName = std::string{replSetString}.substr(0, slash);
    }

    const auto parseSeed = [&seeds, &externalState](const std::string_view seedRaw) {
        if (seedRaw.empty()) {
            return;
        }
        HostAndPort hostAndPort;
        try {
            hostAndPort = HostAndPort::parseThrowing(seedRaw);
        } catch (...) {
            uasserted(10283301, "bad --replSet seed hostname (could not parse to HostAndPort)");
        }
        uassert(10283302,
                "bad --replSet command line config string (has duplicate seeds)",
                std::find(seeds.begin(), seeds.end(), hostAndPort) == seeds.end());
        uassert(
            10283303, "can't use localhost in replset seed host list", !hostAndPort.isLocalHost());

        if (externalState->isSelf(hostAndPort, boost::none, getGlobalServiceContext())) {
            LOGV2_DEBUG(10283304, 1, "Ignoring seed (=self)", "seed"_attr = hostAndPort.toString());
        } else {
            seeds.emplace_back(hostAndPort);
        }
    };

    auto seedsRaw = replSetString.substr(slash + 1);
    while (!seedsRaw.empty()) {
        const auto nextSeparator = seedsRaw.find(',');
        if (nextSeparator == std::string_view::npos) {
            break;
        }
        parseSeed(seedsRaw.substr(0, nextSeparator));
        seedsRaw = seedsRaw.substr(nextSeparator + 1);
    }
    parseSeed(seedsRaw);

    return {setName, seeds};
}
}  // namespace repl
}  // namespace mongo
