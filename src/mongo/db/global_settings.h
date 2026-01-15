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

#pragma once

#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/service_context.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {

// TODO move to access_control module where the only impl is.
class MONGO_MOD_UNFORTUNATELY_OPEN ClusterNetworkRestrictionManager {
public:
    virtual ~ClusterNetworkRestrictionManager() = default;
    virtual void updateClusterNetworkRestrictions() = 0;
    static void set(ServiceContext* service,
                    std::unique_ptr<ClusterNetworkRestrictionManager> manager);
};

struct MONGO_MOD_NEEDS_REPLACEMENT MongodGlobalParams {
    bool scriptingEnabled = true;  // Use "security.javascriptEnabled" to set this variable. Or use
                                   // --noscripting which will set it to false.

    std::shared_ptr<std::vector<std::string>> allowlistedClusterNetwork;
};

MONGO_MOD_NEEDS_REPLACEMENT extern MongodGlobalParams mongodGlobalParams;

// TODO: move these to a replication module.
void setGlobalReplSettings(const repl::ReplSettings& settings);
MONGO_MOD_NEEDS_REPLACEMENT const repl::ReplSettings& getGlobalReplSettings();

}  // namespace mongo
