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

#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
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

class CmdListDatabases : public Command {
public:
    virtual bool slaveOk() const {
        return false;
    }
    virtual bool slaveOverrideOk() const {
        return true;
    }
    virtual bool adminOnly() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void help(stringstream& help) const {
        help << "{ listDatabases:1, [filter: <filterObject>] [, nameOnly: true ] }\n"
                "list databases on this server";
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::listDatabases);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    CmdListDatabases() : Command("listDatabases", true) {}

    bool run(OperationContext* opCtx,
             const string& dbname,
             BSONObj& jsobj,
             string& errmsg,
             BSONObjBuilder& result) {
        // Parse the filter.
        std::unique_ptr<MatchExpression> filter;
        if (auto filterElt = jsobj[kFilterField]) {
            if (filterElt.type() != BSONType::Object) {
                return appendCommandStatus(result,
                                           {ErrorCodes::TypeMismatch,
                                            str::stream() << "Field '" << kFilterField
                                                          << "' must be of type Object in: "
                                                          << jsobj});
            }
            // The collator is null because database metadata objects are compared using simple
            // binary comparison.
            const CollatorInterface* collator = nullptr;
            auto statusWithMatcher = MatchExpressionParser::parse(
                filterElt.Obj(), ExtensionsCallbackDisallowExtensions(), collator);
            if (!statusWithMatcher.isOK()) {
                return appendCommandStatus(result, statusWithMatcher.getStatus());
            }
            filter = std::move(statusWithMatcher.getValue());
        }
        bool nameOnly = jsobj[kNameOnlyField].trueValue();

        vector<string> dbNames;
        StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();
        {
            Lock::GlobalLock lk(opCtx, MODE_IS, UINT_MAX);
            storageEngine->listDatabases(&dbNames);
        }

        vector<BSONObj> dbInfos;

        bool filterNameOnly = filter && filter->isLeaf() && filter->path() == kNameField;
        intmax_t totalSize = 0;
        for (vector<string>::iterator i = dbNames.begin(); i != dbNames.end(); ++i) {
            const string& dbname = *i;

            BSONObjBuilder b;
            b.append("name", dbname);

            int64_t size = 0;
            if (!nameOnly) {
                // Filtering on name only should not require taking locks on filtered-out names.
                if (filterNameOnly && !filter->matchesBSON(b.asTempObj()))
                    continue;

                Lock::DBLock dbLock(opCtx, dbname, MODE_IS);

                Database* db = dbHolder().get(opCtx, dbname);
                if (!db)
                    continue;

                const DatabaseCatalogEntry* entry = db->getDatabaseCatalogEntry();
                invariant(entry);

                size = entry->sizeOnDisk(opCtx);
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
