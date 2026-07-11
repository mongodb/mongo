// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/*
 * Defines a shell command for hashing a BSONElement value
 */

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/hasher.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

#include <iosfwd>
#include <string>

namespace mongo {

using std::string;
using std::stringstream;

// Test-only, enabled via command-line. See docs/test_commands.md.
class CmdHashElt : public BasicCommand {
public:
    CmdHashElt() : BasicCommand("_hashBSONElement") {}

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    // No auth needed because it only works when enabled via command line.
    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();
    }

    bool requiresAuthzChecks() const override {
        return false;
    }

    std::string help() const override {
        return "returns the hash of the first BSONElement val in a BSONObj";
    }

    /* CmdObj has the form {"hash" : <thingToHash>}
     * or {"hash" : <thingToHash>, "seed" : <number> }
     * Result has the form
     * {"key" : <thingTohash>, "seed" : <int>, "out": NumberLong(<hash>)}
     *
     * Example use in the shell:
     *> db.runCommand({hash: "hashthis", seed: 1})
     *> {"key" : "hashthis",
     *>  "seed" : 1,
     *>  "out" : NumberLong(6271151123721111923),
     *>  "ok" : 1 }
     **/
    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        result.appendAs(cmdObj.firstElement(), "key");

        int seed = 0;
        if (cmdObj.hasField("seed")) {
            if (!cmdObj["seed"].isNumber()) {
                CommandHelpers::appendSimpleCommandStatus(
                    result, false /* ok */, "seed must be a number" /* errmsg */);
                return false;
            }
            seed = cmdObj["seed"].safeNumberInt();
        }
        result.append("seed", seed);

        result.append("out", BSONElementHasher::hash64(cmdObj.firstElement(), seed));
        return true;
    }
};
MONGO_REGISTER_COMMAND(CmdHashElt).testOnly().forRouter().forShard();
}  // namespace mongo
