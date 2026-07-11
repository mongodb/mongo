// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {

/* for diagnostic / testing purposes. Enabled via command line. */
class CmdWhatsMySNI : public BasicCommand {
public:
    CmdWhatsMySNI() : BasicCommand("whatsmysni") {}

    std::string help() const override {
        return "internal testing command. Returns an object containing the connection's "
               "advertised SNI name, if any, and false if none is being advertised";
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto sniName = opCtx->getClient()->getSniNameForSession();
        // if no SNI name is advertised, output is false
        if (!sniName) {
            result.append("sni", false);
        } else {
            result.append("sni", *sniName);
        }

        result.append("ok", 1);

        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();
    }

    bool requiresAuthzChecks() const final {
        return false;
    }
};

MONGO_REGISTER_COMMAND(CmdWhatsMySNI).testOnly().forShard();
}  // namespace mongo
