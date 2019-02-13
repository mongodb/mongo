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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/list_databases_gen.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"

namespace mongo {
namespace {
static const StringData kFilterField{"filter"};
static const StringData kNameField{"name"};
static const StringData kNameOnlyField{"nameOnly"};

// Failpoint which causes to hang "listDatabases" cmd after acquiring global lock in IS mode.
MONGO_FAIL_POINT_DEFINE(hangBeforeListDatabases);
}  // namespace

using std::set;
using std::string;
using std::stringstream;
using std::vector;

// XXX: remove and put into storage api
intmax_t dbSize(const string& database);

class CmdListDatabases : public BasicCommand {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kOptIn;
    }
    bool adminOnly() const final {
        return true;
    }
    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }
    std::string help() const final {
        return "{ listDatabases:1, [filter: <filterObject>] [, nameOnly: true ] }\n"
               "list databases on this server";
    }

    /* listDatabases is always authorized,
     * however the results returned will be redacted
     * based on read privileges if auth is enabled
     * and the current user does not have listDatabases permisison.
     */
    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const final {
        return Status::OK();
    }

    CmdListDatabases() : BasicCommand("listDatabases") {}

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
        IDLParserErrorContext ctx("listDatabases");
        auto cmd = ListDatabasesCommand::parse(ctx, cmdObj);
        auto* as = AuthorizationSession::get(opCtx->getClient());

        // {nameOnly: bool} - default false.
        const bool nameOnly = cmd.getNameOnly();

        // {authorizedDatabases: bool} - Dynamic default based on permissions.
        const bool authorizedDatabases = ([as](const boost::optional<bool>& authDB) {
            const bool mayListAllDatabases = as->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::listDatabases);

            if (authDB) {
                uassert(ErrorCodes::Unauthorized,
                        "Insufficient permissions to list all databases",
                        authDB.get() || mayListAllDatabases);
                return authDB.get();
            }

            // By default, list all databases if we can, otherwise
            // only those we're allowed to find on.
            return !mayListAllDatabases;
        })(cmd.getAuthorizedDatabases());

        // {filter: matchExpression}.
        std::unique_ptr<MatchExpression> filter;
        if (auto filterObj = cmd.getFilter()) {
            // The collator is null because database metadata objects are compared using simple
            // binary comparison.
            const CollatorInterface* collator = nullptr;
            boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContext(opCtx, collator));
            auto matcher =
                uassertStatusOK(MatchExpressionParser::parse(filterObj.get(), std::move(expCtx)));
            filter = std::move(matcher);
        }

        vector<string> dbNames;
        StorageEngine* storageEngine = getGlobalServiceContext()->getStorageEngine();
        {
            Lock::GlobalLock lk(opCtx, MODE_IS);
            CurOpFailpointHelpers::waitWhileFailPointEnabled(
                &hangBeforeListDatabases, opCtx, "hangBeforeListDatabases", []() {});
            storageEngine->listDatabases(&dbNames);
        }

        vector<BSONObj> dbInfos;

        const bool filterNameOnly = filter &&
            filter->getCategory() == MatchExpression::MatchCategory::kLeaf &&
            filter->path() == kNameField;
        intmax_t totalSize = 0;
        for (const auto& dbname : dbNames) {
            if (authorizedDatabases && !as->isAuthorizedForAnyActionOnAnyResourceInDB(dbname)) {
                // We don't have listDatabases on the cluser or find on this database.
                continue;
            }

            BSONObjBuilder b;
            b.append("name", dbname);

            int64_t size = 0;
            if (!nameOnly) {
                // Filtering on name only should not require taking locks on filtered-out names.
                if (filterNameOnly && !filter->matchesBSON(b.asTempObj()))
                    continue;

                AutoGetDb autoDb(opCtx, dbname, MODE_IS);
                Database* const db = autoDb.getDb();
                if (!db)
                    continue;

                const DatabaseCatalogEntry* entry = db->getDatabaseCatalogEntry();
                invariant(entry);

                writeConflictRetry(
                    opCtx, "sizeOnDisk", dbname, [&] { size = entry->sizeOnDisk(opCtx); });
                b.append("sizeOnDisk", static_cast<double>(size));

                b.appendBool("empty", entry->isEmpty());
            }
            BSONObj curDbObj = b.obj();

            if (!filter || filter->matchesBSON(curDbObj)) {
                totalSize += size;
                dbInfos.push_back(curDbObj);
            }
        }

        result.append("databases", dbInfos);
        if (!nameOnly) {
            result.append("totalSize", double(totalSize));
        }
        return true;
    }
} cmdListDatabases;
}
