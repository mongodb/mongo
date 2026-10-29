// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/duration.h"
#include "mongo/util/timer.h"

#include <cstdint>
#include <iosfwd>
#include <string>

namespace mongo {

using std::string;
using std::stringstream;

// Test-only, enabled via command line. See docs/test_commands.md.
class CPULoadCommand : public BasicCommand {
public:
    CPULoadCommand() : BasicCommand("cpuload") {}
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "internal. for testing only."
               "{ cpuload : 1, cpuFactor : 1 } Runs a straight CPU load. Length of execution "
               "scaled by cpuFactor. Puts no additional load on the server beyond the cpu use."
               "Useful for testing the stability of the performance of the underlying system,"
               "by running the command repeatedly and observing the variation in execution time.";
    }

    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();  // No auth required
    }

    bool requiresAuthzChecks() const override {
        return false;
    }

    bool run(OperationContext* txn,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        double cpuFactor = 1;
        if (cmdObj["cpuFactor"].isNumber()) {
            cpuFactor = cmdObj["cpuFactor"].number();
        }

        Timer t{};
        long long limit = 10000 * cpuFactor;
        // volatile used to ensure that loop is not optimized away
        volatile uint64_t lresult [[maybe_unused]] = 0;  // NOLINT
        uint64_t x = 100;
        for (long long i = 0; i < limit; i++) {
            x *= 13;
        }
        lresult = x;

        // add time-consuming statistics
        auto micros = t.elapsed();
        result.append("durationMillis", durationCount<Milliseconds>(micros));
        result.append("durationSeconds", durationCount<Seconds>(micros));
        return true;
    }
    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
};

MONGO_REGISTER_COMMAND(CPULoadCommand).testOnly().forRouter().forShard();


}  // namespace mongo
