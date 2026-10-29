// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/replay/replay_config.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

namespace mongo {
class ConfigHandler {
public:
    /*
     * Handle command line arguments.
     * -i <input file> => recording path
     * -t <target> => uri of the mongodb server
     * -c <config> => config file. A list of <recording file>,<uri>. Used for spinning up multiple
     * mongor.
     */
    std::vector<ReplayConfig> parse(int argc, char** argv);

private:
    std::vector<ReplayConfig> parseMultipleInstanceConfig(const std::string& path);
};
}  // namespace mongo
