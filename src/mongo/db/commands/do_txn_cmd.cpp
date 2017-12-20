/**
 * Copyright (C) 2017 MongoDB Inc.
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
#include "mongo/db/repl/do_txn.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
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
 * Returns kNeedsSuperuser, if the provided doTxn command contains
 * an empty doTxn command or createCollection/renameCollection commands are mixed in doTxn
 * batch. Returns kNeedForceAndUseUUID if an operation contains a UUID, and will create a collection
 * with the user-specified UUID. Returns
 * kNeedsUseUUID if the operation contains a UUID. Returns kOk if no conditions
 * which must be specially handled are detected. May throw exceptions if the input is malformed.
 */
OplogApplicationValidity validateDoTxnCommand(const BSONObj& cmdObj) {
    const size_t maxDoTxnDepth = 10;
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

    // Insert the top level doTxn command into the stack.
    toCheck.emplace(std::make_pair(0, cmdObj));

    while (!toCheck.empty()) {
        size_t depth;
        BSONObj doTxnObj;
        std::tie(depth, doTxnObj) = toCheck.top();
        toCheck.pop();

        checkBSONType(BSONType::Array, doTxnObj.firstElement());
        // Check if the doTxn command is empty. This is probably not something that should
        // happen, so require a superuser to do this.
        if (doTxnObj.firstElement().Array().empty()) {
            return OplogApplicationValidity::kNeedsSuperuser;
        }

        // createCollection and renameCollection are only allowed to be applied
        // individually. Ensure there is no create/renameCollection in a batch
        // of size greater than 1.
        if (doTxnObj.firstElement().Array().size() > 1) {
            for (const BSONElement& e : doTxnObj.firstElement().Array()) {
                checkBSONType(BSONType::Object, e);
                auto oplogEntry = e.Obj();
                if (checkCOperationType(oplogEntry, "create"_sd) ||
                    checkCOperationType(oplogEntry, "renameCollection"_sd)) {
                    return OplogApplicationValidity::kNeedsSuperuser;
                }
            }
        }

        // For each doTxn command, iterate the ops.
        for (BSONElement element : doTxnObj.firstElement().Array()) {
            checkBSONType(BSONType::Object, element);
            BSONObj opObj = element.Obj();

            bool opHasUUIDs = operationContainsUUID(opObj);

            if (serverGlobalParams.featureCompatibility.getVersion() ==
                ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo34) {
                uassert(ErrorCodes::OplogOperationUnsupported,
                        "doTxn with UUID requires upgrading to FeatureCompatibilityVersion 3.6",
                        !opHasUUIDs);
            }

            // If the op uses any UUIDs at all then the user must possess extra privileges.
            if (opHasUUIDs && ret == OplogApplicationValidity::kOk)
                ret = OplogApplicationValidity::kNeedsUseUUID;
            if (opHasUUIDs && checkCOperationType(opObj, "create"_sd)) {
                // If the op is 'c' and forces the server to ingest a collection
                // with a specific, user defined UUID.
                ret = OplogApplicationValidity::kNeedsForceAndUseUUID;
            }

            // If the op contains a nested doTxn...
            if (checkCOperationType(opObj, "doTxn"_sd)) {
                // And we've recursed too far, then bail out.
                uassert(ErrorCodes::FailedToParse, "Too many nested doTxn", depth < maxDoTxnDepth);

                // Otherwise, if the op contains an doTxn, but we haven't recursed too far:
                // extract the doTxn command, and insert it into the stack.
                checkBSONType(BSONType::Object, opObj["o"]);
                BSONObj oObj = opObj["o"].Obj();
                toCheck.emplace(std::make_pair(depth + 1, std::move(oObj)));
            }
        }
    }

    return ret;
}

class DoTxnCmd : public BasicCommand {
public:
    DoTxnCmd() : BasicCommand("doTxn") {}

    bool slaveOk() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    void help(std::stringstream& help) const override {
        help << "internal (sharding)\n{ doTxn : [ ] , preCondition : [ { ns : ... , q : ... , "
                "res : ... } ] }";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) override {
        OplogApplicationValidity validity = validateDoTxnCommand(cmdObj);
        return OplogApplicationChecks::checkAuthForCommand(
            opCtx, dbname, cmdObj, validity, OplogApplicationCommand::kDoTxnCmd);
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
        if (!status.isOK()) {
            return appendCommandStatus(result, status);
        }

        // TODO (SERVER-30217): When a write concern is provided to the doTxn command, we
        // normally wait on the OpTime of whichever operation successfully completed last. This is
        // erroneous, however, if the last operation in the array happens to be a write no-op and
        // thus isn’t assigned an OpTime. Let the second to last operation in the doTxn be write
        // A, the last operation in doTxn be write B. Let B do a no-op write and let the
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
            cmdObj, DoTxn::kOplogApplicationModeFieldName, &oplogApplicationModeString);

        if (status.isOK()) {
            auto modeSW = repl::OplogApplication::parseMode(oplogApplicationModeString);
            if (!modeSW.isOK()) {
                // Unable to parse the mode argument.
                return appendCommandStatus(
                    result,
                    modeSW.getStatus().withContext(str::stream() << "Could not parse " +
                                                       DoTxn::kOplogApplicationModeFieldName));
            }
            oplogApplicationMode = modeSW.getValue();
        } else if (status != ErrorCodes::NoSuchKey) {
            // NoSuchKey means the user did not supply a mode.
            return appendCommandStatus(result,
                                       Status(status.code(),
                                              str::stream() << "Could not parse out "
                                                            << DoTxn::kOplogApplicationModeFieldName
                                                            << ": "
                                                            << status.reason()));
        }

        auto doTxnStatus = appendCommandStatus(
            result, doTxn(opCtx, dbname, cmdObj, oplogApplicationMode, &result));

        return doTxnStatus;
    }

} doTxnCmd;

}  // namespace
}  // namespace mongo
