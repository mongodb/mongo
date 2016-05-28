// collection_to_capped.cpp

/**
*    Copyright (C) 2013-2014 MongoDB Inc.
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


#include "mongo/db/background.h"
#include "mongo/db/catalog/capped_utils.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/query/find.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/service_context.h"

namespace mongo {

using std::unique_ptr;
using std::string;
using std::stringstream;

class CmdCloneCollectionAsCapped : public Command {
public:
    CmdCloneCollectionAsCapped() : Command("cloneCollectionAsCapped") {}
    virtual bool slaveOk() const {
        return false;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    virtual void help(stringstream& help) const {
        help << "{ cloneCollectionAsCapped:<fromName>, toCollection:<toName>, size:<sizeInBytes> }";
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet sourceActions;
        sourceActions.addAction(ActionType::find);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), sourceActions));

        ActionSet targetActions;
        targetActions.addAction(ActionType::insert);
        targetActions.addAction(ActionType::createIndex);
        targetActions.addAction(ActionType::convertToCapped);
        std::string collection = cmdObj.getStringField("toCollection");
        uassert(16708, "bad 'toCollection' value", !collection.empty());

        out->push_back(
            Privilege(ResourcePattern::forExactNamespace(NamespaceString(dbname, collection)),
                      targetActions));
    }
    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& jsobj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        string from = jsobj.getStringField("cloneCollectionAsCapped");
        string to = jsobj.getStringField("toCollection");
        double size = jsobj.getField("size").number();
        bool temp = jsobj.getField("temp").trueValue();

        if (from.empty() || to.empty() || size == 0) {
            errmsg = "invalid command spec";
            return false;
        }

        ScopedTransaction transaction(txn, MODE_IX);
        AutoGetDb autoDb(txn, dbname, MODE_X);

        NamespaceString nss(dbname, to);
        if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(nss)) {
            return appendCommandStatus(
                result,
                Status(ErrorCodes::NotMaster,
                       str::stream() << "Not primary while cloning collection " << from << " to "
                                     << to
                                     << " (as capped)"));
        }

        Database* const db = autoDb.getDb();
        if (!db) {
            return appendCommandStatus(
                result,
                Status(ErrorCodes::NamespaceNotFound,
                       str::stream() << "database " << dbname << " not found"));
        }

        Status status = cloneCollectionAsCapped(txn, db, from, to, size, temp);
        return appendCommandStatus(result, status);
    }
} cmdCloneCollectionAsCapped;

/* jan2010:
   Converts the given collection to a capped collection w/ the specified size.
   This command is not highly used, and is not currently supported with sharded
   environments.
   */
class CmdConvertToCapped : public Command {
public:
    CmdConvertToCapped() : Command("convertToCapped") {}
    virtual bool slaveOk() const {
        return false;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    virtual void help(stringstream& help) const {
        help << "{ convertToCapped:<fromCollectionName>, size:<sizeInBytes> }";
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::convertToCapped);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& jsobj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        string shortSource = jsobj.getStringField("convertToCapped");
        double size = jsobj.getField("size").number();

        if (shortSource.empty() || size == 0) {
            errmsg = "invalid command spec";
            return false;
        }

        return appendCommandStatus(
            result, convertToCapped(txn, NamespaceString(dbname, shortSource), size));
    }

} cmdConvertToCapped;
}
