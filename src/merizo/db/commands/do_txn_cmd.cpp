/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kCommand

#include "merizo/platform/basic.h"

#include <vector>

#include "merizo/bson/util/bson_check.h"
#include "merizo/bson/util/bson_extract.h"
#include "merizo/db/auth/authorization_session.h"
#include "merizo/db/catalog/document_validation.h"
#include "merizo/db/catalog/uuid_catalog.h"
#include "merizo/db/client.h"
#include "merizo/db/commands.h"
#include "merizo/db/commands/oplog_application_checks.h"
#include "merizo/db/commands/test_commands_enabled.h"
#include "merizo/db/concurrency/write_conflict_exception.h"
#include "merizo/db/db_raii.h"
#include "merizo/db/dbdirectclient.h"
#include "merizo/db/jsobj.h"
#include "merizo/db/operation_context.h"
#include "merizo/db/repl/do_txn.h"
#include "merizo/db/repl/oplog.h"
#include "merizo/db/repl/oplog_entry_gen.h"
#include "merizo/db/repl/repl_client_info.h"
#include "merizo/db/service_context.h"
#include "merizo/util/log.h"
#include "merizo/util/scopeguard.h"
#include "merizo/util/uuid.h"

namespace merizo {
namespace {

/**
 * Returns kNeedsUseUUID if the operation contains a UUID. Returns kOk if no conditions
 * which must be specially handled are detected. Throws an exception if the input is malformed or
 * if a command is in the list of ops.
 */
OplogApplicationValidity validateDoTxnCommand(const BSONObj& doTxnObj) {
    auto parseOp = [](const BSONObj& opObj) {
        try {
            return repl::ReplOperation::parse(IDLParserErrorContext("doTxn"), opObj);
        } catch (...) {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream() << "cannot apply a malformed operation in doTxn: "
                                    << redact(opObj)
                                    << ": "
                                    << exceptionToStatus().toString());
        }
    };

    OplogApplicationValidity ret = OplogApplicationValidity::kOk;

    checkBSONType(BSONType::Array, doTxnObj.firstElement());
    // Check if the doTxn command is empty.  There's no good reason for an empty transaction,
    // so reject it.
    uassert(ErrorCodes::InvalidOptions,
            "An empty doTxn command is not allowed.",
            !doTxnObj.firstElement().Array().empty());

    // Iterate the ops.
    for (BSONElement element : doTxnObj.firstElement().Array()) {
        checkBSONType(BSONType::Object, element);
        BSONObj opObj = element.Obj();
        auto op = parseOp(opObj);

        // If the op is a command, it's illegal.
        uassert(ErrorCodes::InvalidOptions,
                "Commands cannot be applied via doTxn.",
                op.getOpType() != repl::OpTypeEnum::kCommand);

        // If the op uses any UUIDs at all then the user must possess extra privileges.
        if (op.getUuid()) {
            ret = OplogApplicationValidity::kNeedsUseUUID;
        }
    }
    return ret;
}

class DoTxnCmd : public BasicCommand {
public:
    DoTxnCmd() : BasicCommand("doTxn") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool supportsReadConcern(const std::string& dbName,
                             const BSONObj& cmdObj,
                             repl::ReadConcernLevel level) const {
        // Support the read concerns before and after upconversion.
        return level == repl::ReadConcernLevel::kLocalReadConcern ||
            level == repl::ReadConcernLevel::kSnapshotReadConcern;
    }

    std::string help() const override {
        return "internal (sharding)\n{ doTxn : [ ] , preCondition : [ { ns : ... , q : ... , "
               "res : ... } ] }";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const override {
        OplogApplicationValidity validity = validateDoTxnCommand(cmdObj);
        return OplogApplicationChecks::checkAuthForCommand(opCtx, dbname, cmdObj, validity);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {

        validateDoTxnCommand(cmdObj);

        boost::optional<DisableDocumentValidation> maybeDisableValidation;
        if (shouldBypassDocumentValidationForCommand(cmdObj))
            maybeDisableValidation.emplace(opCtx);

        auto status = OplogApplicationChecks::checkOperationArray(cmdObj.firstElement());
        uassertStatusOK(status);

        // TODO (SERVER-30217): When a write concern is provided to the doTxn command, we
        // normally wait on the OpTime of whichever operation successfully completed last. This is
        // erroneous, however, if the last operation in the array happens to be a write no-op and
        // thus isn’t assigned an OpTime. Let the second to last operation in the doTxn be write
        // A, the last operation in doTxn be write B. Let B do a no-op write and let the
        // operation that caused B to be a no-op be C. If C has an OpTime after A but before B,
        // then we won’t wait for C to be replicated and it could be rolled back, even though B
        // was acknowledged. To fix this, we should wait for replication of the node’s last applied
        // OpTime if the last write operation was a no-op write.

        auto doTxnStatus = CommandHelpers::appendCommandStatusNoThrow(
            result, doTxn(opCtx, dbname, cmdObj, &result));

        return doTxnStatus;
    }
};

MONGO_REGISTER_TEST_COMMAND(DoTxnCmd);

}  // namespace
}  // namespace merizo
