// list_databases.cpp

/**
*    Copyright (C) 2014 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"

namespace mongo {
namespace {
static const StringData kFilterField{"filter"};
static const StringData kNameField{"name"};
static const StringData kNameOnlyField{"nameOnly"};
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
             const BSONObj& jsobj,
             BSONObjBuilder& result) final {
        // Parse the filter.
        std::unique_ptr<MatchExpression> filter;
        if (auto filterElt = jsobj[kFilterField]) {
            if (filterElt.type() != BSONType::Object) {
                uasserted(ErrorCodes::TypeMismatch,
                          str::stream() << "Field '" << kFilterField
                                        << "' must be of type Object in: "
                                        << jsobj);
            }
            // The collator is null because database metadata objects are compared using simple
            // binary comparison.
            const CollatorInterface* collator = nullptr;
            boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContext(opCtx, collator));
            auto statusWithMatcher =
                MatchExpressionParser::parse(filterElt.Obj(), std::move(expCtx));
            uassertStatusOK(statusWithMatcher.getStatus());
            filter = std::move(statusWithMatcher.getValue());
        }
        bool nameOnly = jsobj[kNameOnlyField].trueValue();

        vector<string> dbNames;
        StorageEngine* storageEngine = getGlobalServiceContext()->getStorageEngine();
        {
            Lock::GlobalLock lk(opCtx, MODE_IS);
            storageEngine->listDatabases(&dbNames);
        }

        vector<BSONObj> dbInfos;

        // If we have ActionType::listDatabases,
        // then we don't need to test each record in the output.
        // Otherwise, we'll test the database names as we enumerate them.
        const auto as = AuthorizationSession::get(opCtx->getClient());
        const bool checkAuth = as &&
            !as->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                  ActionType::listDatabases);

        const bool filterNameOnly = filter &&
            filter->getCategory() == MatchExpression::MatchCategory::kLeaf &&
            filter->path() == kNameField;
        intmax_t totalSize = 0;
        for (const auto& dbname : dbNames) {
            if (checkAuth && as &&
                !as->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbname),
                                                      ActionType::find)) {
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
