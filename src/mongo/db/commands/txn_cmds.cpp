/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_catalog.h"

namespace mongo {
namespace {

class CmdCommitTxn : public BasicCommand {
public:
    CmdCommitTxn() : BasicCommand("commitTransaction") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    virtual bool adminOnly() const {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Commits a transaction";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const override {
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto session = OperationContextSession::get(opCtx);
        uassert(
            ErrorCodes::CommandFailed, "commitTransaction must be run within a session", session);

        // commitTransaction is retryable.
        if (session->transactionIsCommitted()) {
            // We set the client last op to the last optime observed by the system to ensure that
            // we wait for the specified write concern on an optime greater than or equal to the
            // commit oplog entry.
            auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
            replClient.setLastOpToSystemLastOpTime(opCtx);
            return true;
        }

        uassert(ErrorCodes::NoSuchTransaction,
                "Transaction isn't in progress",
                session->inMultiDocumentTransaction());

        session->commitTransaction(opCtx);

        return true;
    }

} commitTxn;

class CmdPrepareTxn : public BasicCommand {
public:
    CmdPrepareTxn() : BasicCommand("prepareTransaction") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool adminOnly() const {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Prepares a transaction. This is only expected to be called by mongos.";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const override {
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto session = OperationContextSession::get(opCtx);
        uassert(
            ErrorCodes::CommandFailed, "prepareTransaction must be run within a session", session);

        uassert(ErrorCodes::NoSuchTransaction,
                "Transaction isn't in progress",
                session->inMultiDocumentTransaction());

        session->prepareTransaction(opCtx);
        return true;
    }
};

MONGO_REGISTER_TEST_COMMAND(CmdPrepareTxn);

class CmdAbortTxn : public BasicCommand {
public:
    CmdAbortTxn() : BasicCommand("abortTransaction") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    virtual bool adminOnly() const {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Aborts a transaction";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const override {
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto session = OperationContextSession::get(opCtx);
        uassert(
            ErrorCodes::CommandFailed, "abortTransaction must be run within a session", session);

        // TODO SERVER-33501 Change this when abortTransaction is retryable.
        uassert(ErrorCodes::NoSuchTransaction,
                "Transaction isn't in progress",
                session->inMultiDocumentTransaction());

        session->abortActiveTransaction(opCtx);
        return true;
    }

} abortTxn;

}  // namespace
}  // namespace mongo
