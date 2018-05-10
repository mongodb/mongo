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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/db/catalog/catalog_control.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/util/log.h"

namespace mongo {
/**
 * This testing-only command causes the server to close and reopen the catalog, rebuilding all
 * in-memory data structures.
 */
class RestartCatalogCmd final : public BasicCommand {
public:
    RestartCatalogCmd() : BasicCommand("restartCatalog") {}

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const final {
        // No auth checks as this is a testing-only command.
        return Status::OK();
    }

    bool adminOnly() const final {
        return true;
    }

    bool maintenanceMode() const final {
        return true;
    }

    bool maintenanceOk() const final {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    std::string help() const final {
        return "restart catalog\n"
               "Internal command for testing only. Closes and restores the catalog, rebuilding\n"
               "in-memory data structures as needed.\n";
    }

    bool run(OperationContext* opCtx,
             const std::string& db,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        Lock::GlobalLock global(opCtx, MODE_X);

        // This command will fail without modifying the catalog if there are any databases that are
        // marked drop-pending. (Otherwise, the Database object will be reconstructed when
        // re-opening the catalog, but with the drop pending flag cleared.)
        std::vector<std::string> allDbs;
        getGlobalServiceContext()->getStorageEngine()->listDatabases(&allDbs);
        for (auto&& dbName : allDbs) {
            const auto db = DatabaseHolder::getDatabaseHolder().get(opCtx, dbName);
            if (db->isDropPending(opCtx)) {
                uasserted(ErrorCodes::DatabaseDropPending,
                          str::stream() << "cannot restart the catalog because database " << dbName
                                        << " is pending removal");
            }
        }

        auto restoreOplogPointerGuard = MakeGuard([opCtx]() {
            auto db = DatabaseHolder::getDatabaseHolder().openDb(
                opCtx, NamespaceString::kRsOplogNamespace.db());
            invariant(db, "failed to reopen database after early exit from restartCatalog");

            auto oplog = db->getCollection(opCtx, NamespaceString::kRsOplogNamespace.coll());
            invariant(oplog, "failed to get oplog after early exit from restartCatalog");
            repl::establishOplogCollectionForLogging(opCtx, oplog);
        });

        log() << "Closing database catalog";
        catalog::closeCatalog(opCtx);

        restoreOplogPointerGuard.Dismiss();

        log() << "Reopening database catalog";
        catalog::openCatalog(opCtx);

        return true;
    }
};

MONGO_INITIALIZER(RegisterRestartCatalogCommand)(InitializerContext* ctx) {
    if (getTestCommandsEnabled()) {
        // Leaked intentionally: a Command registers itself when constructed.
        new RestartCatalogCmd();
    }
    return Status::OK();
}
}  // namespace mongo
