/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/operation_context.h"

namespace mongo {

namespace {

class ReapLogicalSessionCacheNowCommand final : public BasicCommand {
    MONGO_DISALLOW_COPYING(ReapLogicalSessionCacheNowCommand);

public:
    ReapLogicalSessionCacheNowCommand() : BasicCommand("reapLogicalSessionCacheNow") {}

    bool slaveOk() const override {
        return true;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    void help(std::stringstream& help) const override {
        help << "force the logical session cache to reap. Test command only.";
    }

    // No auth needed because it only works when enabled via command line.
    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) override {
        return Status::OK();
    }

    virtual bool run(OperationContext* opCtx,
                     const std::string& db,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) override {
        auto cache = LogicalSessionCache::get(opCtx);
        auto client = opCtx->getClient();

        auto res = cache->reapNow(client);
        if (!res.isOK()) {
            return appendCommandStatus(result, res);
        }

        return true;
    }
};

MONGO_INITIALIZER(RegisterReapLogicalSessionCacheNowCommand)(InitializerContext* context) {
    if (Command::testCommandsEnabled) {
        // Leaked intentionally: a Command registers itself when constructed.
        new ReapLogicalSessionCacheNowCommand();
    }
    return Status::OK();
}

}  // namespace

}  // namespace mongo
