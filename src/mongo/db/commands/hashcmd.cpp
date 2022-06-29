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

/*
 * Defines a shell command for hashing a BSONElement value
 */

#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/hasher.h"
#include "mongo/db/jsobj.h"

namespace mongo {

using std::string;
using std::stringstream;

// Testing only, enabled via command-line.
class CmdHashElt : public BasicCommand {
public:
    CmdHashElt() : BasicCommand("_hashBSONElement"){};
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    // No auth needed because it only works when enabled via command line.
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {}
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
             const string& db,
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
            seed = cmdObj["seed"].numberInt();
        }
        result.append("seed", seed);

        result.append("out", BSONElementHasher::hash64(cmdObj.firstElement(), seed));
        return true;
    }
};
MONGO_REGISTER_TEST_COMMAND(CmdHashElt);
}  // namespace mongo
