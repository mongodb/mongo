/**
 * Copyright (C) 2018 MongoDB Inc.
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

#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"

namespace mongo {
/**
 * Testing-only command that invalidates a database's view catalog.
 */
class InvalidateViewCatalogCmd final : public BasicCommand {
public:
    InvalidateViewCatalogCmd() : BasicCommand("invalidateViewCatalog") {}

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const final {
        // No auth checks as this is a testing-only command.
        return Status::OK();
    }

    bool adminOnly() const final {
        return false;
    }

    bool maintenanceMode() const final {
        return true;
    }

    bool maintenanceOk() const final {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    std::string help() const final {
        return "invalidate view catalog\n"
               "Internal command for testing only. Invalidates a database's view catalog,\n"
               "forcing a reload on the next view operation.\n";
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        AutoGetDb dblock(opCtx, dbName, LockMode::MODE_IS);
        auto db = dblock.getDb();
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "database " << dbName << " does not exist",
                db);

        db->getViewCatalog()->invalidate();
        return true;
    }
};

MONGO_REGISTER_TEST_COMMAND(InvalidateViewCatalogCmd);

}  // namespace mongo
