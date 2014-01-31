/**
 *    Copyright (C) 2014 MongoDB, Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/base/initializer.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/db.h"
#include "mongo/tools/mongoadmin_options.h"
#include "mongo/tools/tool.h"

using namespace mongo;
using namespace std;

class AdminTool : public Tool {
public:
    AdminTool() : Tool() {}

    virtual void printHelp(ostream& out) {
        printMongoAdminHelp(&out);
    }

    void printMissingIdIndexes(std::list<std::string> collectionsMissingIdIndex) {
        if (!collectionsMissingIdIndex.empty()) {
            toolInfoLog() << "The following collections were missing _id indexes:";
            for (std::list<std::string>::iterator collName = collectionsMissingIdIndex.begin();
                 collName != collectionsMissingIdIndex.end();
                 collName++) {
                toolInfoLog() << '\t' << *collName;
            }
            
            toolInfoLog() << "To create these indexes run the following commands in a mongo shell:";
            for (std::list<std::string>::iterator collName = collectionsMissingIdIndex.begin();
                 collName != collectionsMissingIdIndex.end();
                 collName++) {
                std::string db = collName->substr(0, collName->find('.'));
                std::string coll = collName->substr(collName->find('.')+1);
                toolInfoLog() << '\t' << "db.getSiblingDB('" << db << "')."
                              << coll << ".ensureIndex({_id: 1}, {unique: true});";
            }
            
        }
        else {
            toolInfoLog() << "No collections were missing _id indexes.";
        }
    }

    std::list<std::string> checkForIdIndexes(const std::list<std::string> collNames) {
        // check each collections indexes for an _id index
        std::list<std::string> collectionsThatLackIdIndex;
        for (std::list<std::string>::const_iterator collName = collNames.begin();
             collName != collNames.end();
             collName++) {
            Client::ReadContext ctx(*collName);

            Collection* collection = ctx.ctx().db()->getCollection(*collName);
            if (collection == NULL) {
                continue;
            }

            if (collection->requiresIdIndex() && !collection->getIndexCatalog()->haveIdIndex()) {
                collectionsThatLackIdIndex.push_back(*collName);
            }
        }

        return collectionsThatLackIdIndex;
    }
        
    bool upgradeCheck() {
        // gather full list of database names
        std::vector<std::string> dbNames;
        getDatabaseNames(dbNames);

        // run database-level checks

        // gather full list of collection names
        std::list<std::string> collNames;
        for (std::vector<std::string>::const_iterator dbName = dbNames.begin();
             dbName < dbNames.end();
             dbName++) {
            Client::ReadContext ctx(*dbName);
            Database* db = cc().database();
            db->namespaceIndex().getNamespaces(collNames, /* onlyCollections */ true);
        }

        // run collection-level checks
        printMissingIdIndexes(checkForIdIndexes(collNames));
        return true;
    }

    int run() {
        Client::initThread("adminTool");
        if (mongoAdminGlobalParams.upgradeCheck) {
            if (upgradeCheck()) {
                return EXIT_SUCCESS;
            }
            else {
                return EXIT_FAILURE;
            }
        }
        return EXIT_SUCCESS;
    }
};

REGISTER_MONGO_TOOL(AdminTool);
