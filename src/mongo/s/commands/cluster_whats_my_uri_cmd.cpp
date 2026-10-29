// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/net/hostandport.h"

#include <string>

namespace mongo {
namespace {

class WhatsMyUriCmd : public BasicCommand {
public:
    WhatsMyUriCmd() : BasicCommand("whatsmyuri") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "{whatsmyuri:1}";
    }

    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        // No explicit privileges required.  Any authenticated user may call.
        return Status::OK();
    }

    bool run(OperationContext*,
             const DatabaseName&,
             const BSONObj&,
             BSONObjBuilder& result) override {
        result << "you" << cc().getRemote().toString();
        return true;
    }
};
MONGO_REGISTER_COMMAND(WhatsMyUriCmd).forRouter();

}  // namespace
}  // namespace mongo
