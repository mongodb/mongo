/** @file touch.cpp
    compaction of deleted space in pdfiles (datafiles)
*/

/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,b
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

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/timer.h"
#include "mongo/util/touch_pages.h"

namespace mongo {

using std::string;
using std::stringstream;

class TouchCmd : public Command {
public:
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool adminOnly() const {
        return false;
    }
    virtual bool slaveOk() const {
        return true;
    }
    virtual bool maintenanceMode() const {
        return true;
    }
    virtual void help(stringstream& help) const {
        help << "touch collection\n"
                "Page in all pages of memory containing every extent for the given collection\n"
                "{ touch : <collection_name>, [data : true] , [index : true] }\n"
                " at least one of data or index must be true; default is both are false\n";
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::touch);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    TouchCmd() : Command("touch") {}

    virtual bool run(OperationContext* txn,
                     const string& dbname,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        const NamespaceString nss = parseNsCollectionRequired(dbname, cmdObj);
        if (!nss.isNormal()) {
            errmsg = "bad namespace name";
            return false;
        }

        bool touch_indexes(cmdObj["index"].trueValue());
        bool touch_data(cmdObj["data"].trueValue());

        if (!(touch_indexes || touch_data)) {
            errmsg = "must specify at least one of (data:true, index:true)";
            return false;
        }

        AutoGetCollectionForRead context(txn, nss);

        Collection* collection = context.getCollection();
        if (!collection) {
            errmsg = "collection not found";
            return false;
        }

        return appendCommandStatus(result,
                                   collection->touch(txn, touch_data, touch_indexes, &result));
    }
};
static TouchCmd touchCmd;
}
