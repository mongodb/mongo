/**
 * Copyright (C) 2013 MongoDB Inc.
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

#include <string>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/service_context.h"

namespace mongo {

using std::string;
using std::stringstream;

class AppendOplogNoteCmd : public Command {
public:
    AppendOplogNoteCmd() : Command("appendOplogNote") {}
    virtual bool slaveOk() const {
        return false;
    }
    virtual bool adminOnly() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    virtual void help(stringstream& help) const {
        help << "Adds a no-op entry to the oplog";
    }
    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::appendOplogNote)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }
    virtual bool run(OperationContext* txn,
                     const string& dbname,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        if (!repl::getGlobalReplicationCoordinator()->isReplEnabled()) {
            return appendCommandStatus(
                result,
                Status(ErrorCodes::NoReplicationEnabled,
                       "Must have replication set up to run \"appendOplogNote\""));
        }
        BSONElement dataElement;
        Status status = bsonExtractTypedField(cmdObj, "data", Object, &dataElement);
        if (!status.isOK()) {
            return appendCommandStatus(result, status);
        }

        ScopedTransaction scopedXact(txn, MODE_X);
        Lock::GlobalWrite globalWrite(txn->lockState());

        WriteUnitOfWork wuow(txn);
        getGlobalServiceContext()->getOpObserver()->onOpMessage(txn, dataElement.Obj());
        wuow.commit();
        return true;
    }

} appendOplogNoteCmd;

}  // namespace mongo
