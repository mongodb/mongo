/**
 *    Copyright (C) 2008-2016 MongoDB Inc.
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

#include <vector>

#include "mongo/bson/util/bson_check.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/oplog_application_checks.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/service_context.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

bool checkCOperationType(const BSONObj& opObj, const StringData opName) {
    BSONElement opTypeElem = opObj["op"];
    checkBSONType(BSONType::String, opTypeElem);
    const StringData opType = opTypeElem.checkAndGetStringData();

    if (opType == "c"_sd) {
        BSONElement oElem = opObj["o"];
        checkBSONType(BSONType::Object, oElem);
        BSONObj o = oElem.Obj();

        if (o.firstElement().fieldNameStringData() == opName) {
            return true;
        }
    }
    return false;
};

/**
 * Returns kNeedsSuperuser, if the provided applyOps command contains an empty applyOps command or
 * createCollection/renameCollection commands are mixed in applyOps batch.
 *
 * Returns kNeedForceAndUseUUID if an operation contains a UUID, and will create a collection with
 * the user-specified UUID.
 *
 * Returns kNeedsUseUUID if the operation contains a UUID.
 *
 * Returns kOk if no conditions which must be specially handled are detected.
 *
 * May throw exceptions if the input is malformed.
 */
OplogApplicationValidity validateApplyOpsCommand(const BSONObj& cmdObj) {
    const size_t maxApplyOpsDepth = 10;
    std::stack<std::pair<size_t, BSONObj>> toCheck;

    auto operationContainsUUID = [](const BSONObj& opObj) {
        auto anyTopLevelElementIsUUID = [](const BSONObj& opObj) {
            for (const BSONElement opElement : opObj) {
                if (opElement.type() == BSONType::BinData &&
                    opElement.binDataType() == BinDataType::newUUID) {
                    return true;
                }
            }
            return false;
        };
        if (anyTopLevelElementIsUUID(opObj)) {
            return true;
        }

        BSONElement opTypeElem = opObj["op"];
        checkBSONType(BSONType::String, opTypeElem);
        const StringData opType = opTypeElem.checkAndGetStringData();

        if (opType == "c"_sd) {
            BSONElement oElem = opObj["o"];
            checkBSONType(BSONType::Object, oElem);
            BSONObj o = oElem.Obj();

            if (anyTopLevelElementIsUUID(o)) {
                return true;
            }
        }

        return false;
    };

    OplogApplicationValidity ret = OplogApplicationValidity::kOk;

    // Insert the top level applyOps command into the stack.
    toCheck.emplace(std::make_pair(0, cmdObj));

    while (!toCheck.empty()) {
        size_t depth;
        BSONObj applyOpsObj;
        std::tie(depth, applyOpsObj) = toCheck.top();
        toCheck.pop();

        checkBSONType(BSONType::Array, applyOpsObj.firstElement());
        // Check if the applyOps command is empty. This is probably not something that should
        // happen, so require a superuser to do this.
        if (applyOpsObj.firstElement().Array().empty()) {
            return OplogApplicationValidity::kNeedsSuperuser;
        }

        // createCollection and renameCollection are only allowed to be applied
        // individually. Ensure there is no create/renameCollection in a batch
        // of size greater than 1.
        if (applyOpsObj.firstElement().Array().size() > 1) {
            for (const BSONElement& e : applyOpsObj.firstElement().Array()) {
                checkBSONType(BSONType::Object, e);
                auto oplogEntry = e.Obj();
                if (checkCOperationType(oplogEntry, "create"_sd) ||
                    checkCOperationType(oplogEntry, "renameCollection"_sd)) {
                    return OplogApplicationValidity::kNeedsSuperuser;
                }
            }
        }

        // For each applyOps command, iterate the ops.
        for (BSONElement element : applyOpsObj.firstElement().Array()) {
            checkBSONType(BSONType::Object, element);
            BSONObj opObj = element.Obj();

            bool opHasUUIDs = operationContainsUUID(opObj);

            // If the op uses any UUIDs at all then the user must possess extra privileges.
            if (opHasUUIDs && ret == OplogApplicationValidity::kOk)
                ret = OplogApplicationValidity::kNeedsUseUUID;
            if (opHasUUIDs && checkCOperationType(opObj, "create"_sd)) {
                // If the op is 'c' and forces the server to ingest a collection
                // with a specific, user defined UUID.
                ret = OplogApplicationValidity::kNeedsForceAndUseUUID;
            }

            // If the op contains a nested applyOps...
            if (checkCOperationType(opObj, "applyOps"_sd)) {
                // And we've recursed too far, then bail out.
                uassert(ErrorCodes::FailedToParse,
                        "Too many nested applyOps",
                        depth < maxApplyOpsDepth);

                // Otherwise, if the op contains an applyOps, but we haven't recursed too far:
                // extract the applyOps command, and insert it into the stack.
                checkBSONType(BSONType::Object, opObj["o"]);
                BSONObj oObj = opObj["o"].Obj();
                toCheck.emplace(std::make_pair(depth + 1, std::move(oObj)));
            }
        }
    }

    return ret;
}

class ApplyOpsCmd : public BasicCommand {
public:
    ApplyOpsCmd() : BasicCommand("applyOps") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "internal (sharding)\n{ applyOps : [ ] , preCondition : [ { ns : ... , q : ... , "
               "res : ... } ] }";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const override {
        OplogApplicationValidity validity = validateApplyOpsCommand(cmdObj);
        return OplogApplicationChecks::checkAuthForCommand(opCtx, dbname, cmdObj, validity);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        validateApplyOpsCommand(cmdObj);

        boost::optional<DisableDocumentValidation> maybeDisableValidation;
        if (shouldBypassDocumentValidationForCommand(cmdObj))
            maybeDisableValidation.emplace(opCtx);

        auto status = OplogApplicationChecks::checkOperationArray(cmdObj.firstElement());
        if (!status.isOK()) {
            return CommandHelpers::appendCommandStatus(result, status);
        }

        // TODO (SERVER-30217): When a write concern is provided to the applyOps command, we
        // normally wait on the OpTime of whichever operation successfully completed last. This is
        // erroneous, however, if the last operation in the array happens to be a write no-op and
        // thus isn’t assigned an OpTime. Let the second to last operation in the applyOps be write
        // A, the last operation in applyOps be write B. Let B do a no-op write and let the
        // operation that caused B to be a no-op be C. If C has an OpTime after A but before B,
        // then we won’t wait for C to be replicated and it could be rolled back, even though B
        // was acknowledged. To fix this, we should wait for replication of the node’s last applied
        // OpTime if the last write operation was a no-op write.

        // We set the OplogApplication::Mode argument based on the mode argument given in the
        // command object. If no mode is given, default to the 'kApplyOpsCmd' mode.
        repl::OplogApplication::Mode oplogApplicationMode =
            repl::OplogApplication::Mode::kApplyOpsCmd;  // the default mode.
        std::string oplogApplicationModeString;
        status = bsonExtractStringField(
            cmdObj, repl::ApplyOps::kOplogApplicationModeFieldName, &oplogApplicationModeString);

        if (status.isOK()) {
            auto modeSW = repl::OplogApplication::parseMode(oplogApplicationModeString);
            if (!modeSW.isOK()) {
                // Unable to parse the mode argument.
                return CommandHelpers::appendCommandStatus(
                    result,
                    modeSW.getStatus().withContext(
                        str::stream()
                        << "Could not parse " + repl::ApplyOps::kOplogApplicationModeFieldName));
            }
            oplogApplicationMode = modeSW.getValue();
        } else if (status != ErrorCodes::NoSuchKey) {
            // NoSuchKey means the user did not supply a mode.
            return CommandHelpers::appendCommandStatus(
                result,
                status.withContext(str::stream()
                                   << "Could not parse out "
                                   << repl::ApplyOps::kOplogApplicationModeFieldName));
        }

        auto applyOpsStatus = CommandHelpers::appendCommandStatusNoThrow(
            result, repl::applyOps(opCtx, dbname, cmdObj, oplogApplicationMode, &result));

        return applyOpsStatus;
    }

} applyOpsCmd;

}  // namespace
}  // namespace mongo
