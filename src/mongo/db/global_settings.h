// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/service_context.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {

// TODO(Ignore linting) move to access_control module where the only impl is.
class [[MONGO_MOD_UNFORTUNATELY_OPEN]] ClusterNetworkRestrictionManager {
public:
    virtual ~ClusterNetworkRestrictionManager() = default;
    virtual void updateClusterNetworkRestrictions() = 0;
    static void set(ServiceContext* service,
                    std::unique_ptr<ClusterNetworkRestrictionManager> manager);
};

struct [[MONGO_MOD_NEEDS_REPLACEMENT]] MongodGlobalParams {
    bool scriptingEnabled = true;  // Use "security.javascriptEnabled" to set this variable. Or use
                                   // --noscripting which will set it to false.

    std::shared_ptr<std::vector<std::string>> allowlistedClusterNetwork;
};

[[MONGO_MOD_NEEDS_REPLACEMENT]] extern MongodGlobalParams mongodGlobalParams;

// TODO(SERVER-113439) move these to a replication module.
[[MONGO_MOD_NEEDS_REPLACEMENT]] void setGlobalReplSettings(const repl::ReplSettings& settings);
[[MONGO_MOD_NEEDS_REPLACEMENT]] const repl::ReplSettings& getGlobalReplSettings();

}  // namespace mongo
