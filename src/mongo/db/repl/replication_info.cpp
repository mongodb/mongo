/**
*    Copyright (C) 2008-2014 MongoDB Inc.
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
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kFTDC

#include "mongo/platform/basic.h"

#include <list>
#include <vector>

#include "mongo/client/connpool.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/network_interface.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/metadata/client_metadata_ismaster.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/map_util.h"

namespace mongo {

using std::unique_ptr;
using std::list;
using std::string;
using std::stringstream;

namespace repl {

namespace {

MONGO_FP_DECLARE(impersonateFullyUpgradedFutureVersion);

}  // namespace

void appendReplicationInfo(OperationContext* opCtx, BSONObjBuilder& result, int level) {
    ReplicationCoordinator* replCoord = ReplicationCoordinator::get(opCtx);
    if (replCoord->getSettings().usingReplSets()) {
        IsMasterResponse isMasterResponse;
        replCoord->fillIsMasterForReplSet(&isMasterResponse);
        result.appendElements(isMasterResponse.toBSON());
        if (level) {
            replCoord->appendSlaveInfoData(&result);
        }
        return;
    }

    result.appendBool("ismaster",
                      ReplicationCoordinator::get(opCtx)->isMasterForReportingPurposes());

    if (level) {
        BSONObjBuilder sources(result.subarrayStart("sources"));

        int n = 0;
        list<BSONObj> src;
        {
            const NamespaceString localSources{"local.sources"};
            AutoGetCollectionForReadCommand ctx(opCtx, localSources);
            auto exec = InternalPlanner::collectionScan(
                opCtx, localSources.ns(), ctx.getCollection(), PlanExecutor::NO_YIELD);
            BSONObj obj;
            PlanExecutor::ExecState state;
            while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, NULL))) {
                src.push_back(obj.getOwned());
            }

            // Non-yielding collection scans from InternalPlanner will never error.
            invariant(PlanExecutor::IS_EOF == state);
        }

        for (list<BSONObj>::const_iterator i = src.begin(); i != src.end(); i++) {
            BSONObj s = *i;
            BSONObjBuilder bb;
            bb.append(s["host"]);
            string sourcename = s["source"].valuestr();
            if (sourcename != "main")
                bb.append(s["source"]);
            {
                BSONElement e = s["syncedTo"];
                BSONObjBuilder t(bb.subobjStart("syncedTo"));
                t.appendDate("time", e.timestampTime());
                t.append("inc", e.timestampInc());
                t.done();
            }

            if (level > 1) {
                invariant(!opCtx->lockState()->isLocked());
                // note: there is no so-style timeout on this connection; perhaps we should have
                // one.
                ScopedDbConnection conn(s["host"].valuestr());

                DBClientConnection* cliConn = dynamic_cast<DBClientConnection*>(&conn.conn());
                if (cliConn && replAuthenticate(cliConn)) {
                    BSONObj first = conn->findOne((string) "local.oplog.$" + sourcename,
                                                  Query().sort(BSON("$natural" << 1)));
                    BSONObj last = conn->findOne((string) "local.oplog.$" + sourcename,
                                                 Query().sort(BSON("$natural" << -1)));
                    bb.appendDate("masterFirst", first["ts"].timestampTime());
                    bb.appendDate("masterLast", last["ts"].timestampTime());
                    const auto lag = (last["ts"].timestampTime() - s["syncedTo"].timestampTime());
                    bb.append("lagSeconds", durationCount<Milliseconds>(lag) / 1000.0);
                }
                conn.done();
            }

            sources.append(BSONObjBuilder::numStr(n++), bb.obj());
        }

        sources.done();

        replCoord->appendSlaveInfoData(&result);
    }
}

class ReplicationInfoServerStatus : public ServerStatusSection {
public:
    ReplicationInfoServerStatus() : ServerStatusSection("repl") {}
    bool includeByDefault() const {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement& configElement) const {
        if (!ReplicationCoordinator::get(opCtx)->isReplEnabled()) {
            return BSONObj();
        }

        int level = configElement.numberInt();

        BSONObjBuilder result;
        appendReplicationInfo(opCtx, result, level);

        auto rbid = ReplicationProcess::get(opCtx)->getRollbackID();
        if (ReplicationProcess::kUninitializedRollbackId != rbid) {
            result.append("rbid", rbid);
        }

        return result.obj();
    }

} replicationInfoServerStatus;

class OplogInfoServerStatus : public ServerStatusSection {
public:
    OplogInfoServerStatus() : ServerStatusSection("oplog") {}
    bool includeByDefault() const {
        return false;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement& configElement) const {
        ReplicationCoordinator* replCoord = ReplicationCoordinator::get(opCtx);
        if (!replCoord->isReplEnabled()) {
            return BSONObj();
        }

        BSONObjBuilder result;
        // TODO(siyuan) Output term of OpTime
        result.append("latestOptime", replCoord->getMyLastAppliedOpTime().getTimestamp());

        BSONObj o;
        uassert(17347,
                "Problem reading earliest entry from oplog",
                Helpers::getSingleton(opCtx, NamespaceString::kRsOplogNamespace.ns().c_str(), o));
        result.append("earliestOptime", o["ts"].timestamp());
        return result.obj();
    }
} oplogInfoServerStatus;

class CmdIsMaster : public BasicCommand {
public:
    bool requiresAuth() const override {
        return false;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    std::string help() const override {
        return "Check if this server is primary for a replica set\n"
               "{ isMaster : 1 }";
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {}  // No auth required
    CmdIsMaster() : BasicCommand("isMaster", "ismaster") {}
    virtual bool run(OperationContext* opCtx,
                     const string&,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        /* currently request to arbiter is (somewhat arbitrarily) an ismaster request that is not
           authenticated.
        */
        if (cmdObj["forShell"].trueValue()) {
            LastError::get(opCtx->getClient()).disable();
        }

        transport::Session::TagMask sessionTagsToSet = 0;
        transport::Session::TagMask sessionTagsToUnset = 0;

        // Tag connections to avoid closing them on stepdown.
        auto hangUpElement = cmdObj["hangUpOnStepDown"];
        if (!hangUpElement.eoo() && !hangUpElement.trueValue()) {
            sessionTagsToSet |= transport::Session::kKeepOpen;
        }

        auto& clientMetadataIsMasterState = ClientMetadataIsMasterState::get(opCtx->getClient());
        bool seenIsMaster = clientMetadataIsMasterState.hasSeenIsMaster();
        if (!seenIsMaster) {
            clientMetadataIsMasterState.setSeenIsMaster();
        }

        BSONElement element = cmdObj[kMetadataDocumentName];
        if (!element.eoo()) {
            if (seenIsMaster) {
                uasserted(ErrorCodes::ClientMetadataCannotBeMutated,
                          "The client metadata document may only be sent in the first isMaster");
            }

            auto swParseClientMetadata = ClientMetadata::parse(element);

            uassertStatusOK(swParseClientMetadata.getStatus());

            invariant(swParseClientMetadata.getValue());

            swParseClientMetadata.getValue().get().logClientMetadata(opCtx->getClient());

            clientMetadataIsMasterState.setClientMetadata(
                opCtx->getClient(), std::move(swParseClientMetadata.getValue()));
        }

        // Parse the optional 'internalClient' field. This is provided by incoming connections from
        // mongod and mongos.
        auto internalClientElement = cmdObj["internalClient"];
        if (internalClientElement) {
            sessionTagsToSet |= transport::Session::kInternalClient;

            uassert(ErrorCodes::TypeMismatch,
                    str::stream() << "'internalClient' must be of type Object, but was of type "
                                  << typeName(internalClientElement.type()),
                    internalClientElement.type() == BSONType::Object);

            bool foundMaxWireVersion = false;
            for (auto&& elem : internalClientElement.Obj()) {
                auto fieldName = elem.fieldNameStringData();
                if (fieldName == "minWireVersion") {
                    // We do not currently use 'internalClient.minWireVersion'.
                    continue;
                } else if (fieldName == "maxWireVersion") {
                    foundMaxWireVersion = true;

                    uassert(ErrorCodes::TypeMismatch,
                            str::stream() << "'maxWireVersion' field of 'internalClient' must be "
                                             "of type int, but was of type "
                                          << typeName(elem.type()),
                            elem.type() == BSONType::NumberInt);

                    // All incoming connections from mongod/mongos of earlier versions should be
                    // closed if the featureCompatibilityVersion is bumped to 3.6.
                    if (elem.numberInt() >=
                        WireSpec::instance().incomingInternalClient.maxWireVersion) {
                        sessionTagsToSet |=
                            transport::Session::kLatestVersionInternalClientKeepOpen;
                    } else {
                        sessionTagsToUnset |=
                            transport::Session::kLatestVersionInternalClientKeepOpen;
                    }
                } else {
                    uasserted(ErrorCodes::BadValue,
                              str::stream() << "Unrecognized field of 'internalClient': '"
                                            << fieldName
                                            << "'");
                }
            }

            uassert(ErrorCodes::BadValue,
                    "Missing required field 'maxWireVersion' of 'internalClient'",
                    foundMaxWireVersion);
        } else {
            sessionTagsToUnset |= (transport::Session::kInternalClient |
                                   transport::Session::kLatestVersionInternalClientKeepOpen);
            sessionTagsToSet |= transport::Session::kExternalClientKeepOpen;
        }

        auto session = opCtx->getClient()->session();
        if (session) {
            session->mutateTags(
                [sessionTagsToSet, sessionTagsToUnset](transport::Session::TagMask originalTags) {
                    // After a mongos sends the initial "isMaster" command with its mongos client
                    // information, it sometimes sends another "isMaster" command that is forwarded
                    // from its client. Once kInternalClient has been set, we assume that any future
                    // "isMaster" commands are forwarded in this manner, and we do not update the
                    // session tags.
                    if ((originalTags & transport::Session::kInternalClient) == 0) {
                        return (originalTags | sessionTagsToSet) & ~sessionTagsToUnset;
                    } else {
                        return originalTags;
                    }
                });
        }

        appendReplicationInfo(opCtx, result, 0);

        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            const int configServerModeNumber = 2;
            result.append("configsvr", configServerModeNumber);
        }

        result.appendNumber("maxBsonObjectSize", BSONObjMaxUserSize);
        result.appendNumber("maxMessageSizeBytes", MaxMessageSizeBytes);
        result.appendNumber("maxWriteBatchSize", write_ops::kMaxWriteBatchSize);
        result.appendDate("localTime", jsTime());
        result.append("logicalSessionTimeoutMinutes", localLogicalSessionTimeoutMinutes);

        if (MONGO_FAIL_POINT(impersonateFullyUpgradedFutureVersion)) {
            result.append("minWireVersion", WireVersion::FUTURE_WIRE_VERSION_FOR_TESTING);
            result.append("maxWireVersion", WireVersion::FUTURE_WIRE_VERSION_FOR_TESTING);
        } else if (internalClientElement) {
            result.append("minWireVersion",
                          WireSpec::instance().incomingInternalClient.minWireVersion);
            result.append("maxWireVersion",
                          WireSpec::instance().incomingInternalClient.maxWireVersion);
        } else {
            result.append("minWireVersion",
                          WireSpec::instance().incomingExternalClient.minWireVersion);
            result.append("maxWireVersion",
                          WireSpec::instance().incomingExternalClient.maxWireVersion);
        }

        result.append("readOnly", storageGlobalParams.readOnly);

        const auto parameter = mapFindWithDefault(ServerParameterSet::getGlobal()->getMap(),
                                                  "automationServiceDescriptor",
                                                  static_cast<ServerParameter*>(nullptr));
        if (parameter)
            parameter->append(opCtx, result, "automationServiceDescriptor");

        if (opCtx->getClient()->session()) {
            MessageCompressorManager::forSession(opCtx->getClient()->session())
                .serverNegotiate(cmdObj, &result);
        }

        auto& saslMechanismRegistry = SASLServerMechanismRegistry::get(opCtx->getServiceContext());
        saslMechanismRegistry.advertiseMechanismNamesForUser(opCtx, cmdObj, &result);

        return true;
    }
} cmdismaster;

OpCounterServerStatusSection replOpCounterServerStatusSection("opcountersRepl", &replOpCounters);

}  // namespace repl
}  // namespace mongo
