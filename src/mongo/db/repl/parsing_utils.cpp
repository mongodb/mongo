/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/repl/parsing_utils.h"

#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {
namespace repl {
std::tuple<std::string, std::vector<HostAndPort>> parseReplSetSeedList(
    ReplicationCoordinatorExternalState* externalState, const StringData replSetString) {
    // TODO SERVER-106540: This uassert fails in sharding and maintenance js test suites.
    // Once behavior is specified (on whether an empty string can be passed to this function),
    // we can enable or remove this assert.
    // uassert(10283305, "bad --replSet command line config string (empty)",
    // !replSetString.empty());

    std::string setName;
    std::vector<HostAndPort> seeds;

    const auto slash = replSetString.find('/');
    if (slash == StringData::npos) {
        // replSet name with no seed hosts is valid.
        return {std::string{replSetString}, seeds};
    } else {
        setName = std::string{replSetString}.substr(0, slash);
    }

    const auto parseSeed = [&seeds, &externalState](const StringData seedRaw) {
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

        if (externalState->isSelf(hostAndPort, getGlobalServiceContext())) {
            LOGV2_DEBUG(10283304, 1, "Ignoring seed (=self)", "seed"_attr = hostAndPort.toString());
        } else {
            seeds.emplace_back(hostAndPort);
        }
    };

    auto seedsRaw = replSetString.substr(slash + 1);
    while (!seedsRaw.empty()) {
        const auto nextSeparator = seedsRaw.find(',');
        if (nextSeparator == StringData::npos) {
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
