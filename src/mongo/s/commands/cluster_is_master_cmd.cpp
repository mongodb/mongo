/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/wire_version.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/metadata/client_metadata_ismaster.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/s/mongos_topology_coordinator.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/util/map_util.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/version.h"

namespace mongo {

// Hangs in the beginning of each isMaster command when set.
MONGO_FAIL_POINT_DEFINE(waitInIsMaster);

namespace {

class CmdIsMaster : public BasicCommandWithReplyBuilderInterface {
public:
    CmdIsMaster() : BasicCommandWithReplyBuilderInterface("isMaster", "ismaster") {}

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return "Status information for clients negotiating a connection with this server";
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        // No auth required
    }

    bool requiresAuth() const override {
        return false;
    }

    bool runWithReplyBuilder(OperationContext* opCtx,
                             const std::string& dbname,
                             const BSONObj& cmdObj,
                             rpc::ReplyBuilderInterface* replyBuilder) final {
        CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);

        waitInIsMaster.pauseWhileSet(opCtx);

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

            swParseClientMetadata.getValue().get().setMongoSMetadata(
                getHostNameCachedAndPort(),
                opCtx->getClient()->clientAddress(true),
                VersionInfoInterface::instance().version());

            clientMetadataIsMasterState.setClientMetadata(
                opCtx->getClient(), std::move(swParseClientMetadata.getValue()));
        }

        // If a client is following the awaitable isMaster protocol, maxAwaitTimeMS should be
        // present if and only if topologyVersion is present in the request.
        auto topologyVersionElement = cmdObj["topologyVersion"];
        auto maxAwaitTimeMSField = cmdObj["maxAwaitTimeMS"];
        boost::optional<TopologyVersion> clientTopologyVersion;
        boost::optional<long long> maxAwaitTimeMS;
        if (topologyVersionElement && maxAwaitTimeMSField) {
            clientTopologyVersion = TopologyVersion::parse(IDLParserErrorContext("TopologyVersion"),
                                                           topologyVersionElement.Obj());
            uassert(51758,
                    "topologyVersion must have a non-negative counter",
                    clientTopologyVersion->getCounter() >= 0);

            long long parsedMaxAwaitTimeMS;
            uassertStatusOK(
                bsonExtractIntegerField(cmdObj, "maxAwaitTimeMS", &parsedMaxAwaitTimeMS));

            uassert(
                51759, "maxAwaitTimeMS must be a non-negative integer", parsedMaxAwaitTimeMS >= 0);

            maxAwaitTimeMS = parsedMaxAwaitTimeMS;

            LOGV2_DEBUG(23871, 3, "Using maxAwaitTimeMS for awaitable isMaster protocol.");
        } else {
            uassert(51760,
                    (topologyVersionElement
                         ? "A request with a 'topologyVersion' must include 'maxAwaitTimeMS'"
                         : "A request with 'maxAwaitTimeMS' must include a 'topologyVersion'"),
                    !topologyVersionElement && !maxAwaitTimeMSField);
        }

        auto result = replyBuilder->getBodyBuilder();
        const auto* mongosTopCoord = MongosTopologyCoordinator::get(opCtx);

        auto mongosIsMasterResponse =
            mongosTopCoord->awaitIsMasterResponse(opCtx, clientTopologyVersion, maxAwaitTimeMS);

        mongosIsMasterResponse->appendToBuilder(&result);
        // The isMaster response always includes a topologyVersion.
        auto currentMongosTopologyVersion = mongosIsMasterResponse->getTopologyVersion();

        result.appendNumber("maxBsonObjectSize", BSONObjMaxUserSize);
        result.appendNumber("maxMessageSizeBytes", MaxMessageSizeBytes);
        result.appendNumber("maxWriteBatchSize", write_ops::kMaxWriteBatchSize);
        result.appendDate("localTime", jsTime());
        result.append("logicalSessionTimeoutMinutes", localLogicalSessionTimeoutMinutes);
        result.appendNumber("connectionId", opCtx->getClient()->getConnectionId());

        // Mongos tries to keep exactly the same version range of the server for which
        // it is compiled.
        result.append("maxWireVersion", WireSpec::instance().incomingExternalClient.maxWireVersion);
        result.append("minWireVersion", WireSpec::instance().incomingExternalClient.minWireVersion);

        const auto parameter = mapFindWithDefault(ServerParameterSet::getGlobal()->getMap(),
                                                  "automationServiceDescriptor",
                                                  static_cast<ServerParameter*>(nullptr));
        if (parameter)
            parameter->append(opCtx, result, "automationServiceDescriptor");

        MessageCompressorManager::forSession(opCtx->getClient()->session())
            .serverNegotiate(cmdObj, &result);

        auto& saslMechanismRegistry = SASLServerMechanismRegistry::get(opCtx->getServiceContext());
        saslMechanismRegistry.advertiseMechanismNamesForUser(opCtx, cmdObj, &result);

        if (opCtx->isExhaust()) {
            LOGV2_DEBUG(23872, 3, "Using exhaust for isMaster protocol");

            uassert(51763,
                    "An isMaster request with exhaust must specify 'maxAwaitTimeMS'",
                    maxAwaitTimeMSField);
            invariant(clientTopologyVersion);

            InExhaustIsMaster::get(opCtx->getClient()->session().get())
                ->setInExhaustIsMaster(true /* inExhaustIsMaster */);

            if (clientTopologyVersion->getProcessId() ==
                    currentMongosTopologyVersion.getProcessId() &&
                clientTopologyVersion->getCounter() == currentMongosTopologyVersion.getCounter()) {
                // Indicate that an exhaust message should be generated and the previous BSONObj
                // command parameters should be reused as the next BSONObj command parameters.
                replyBuilder->setNextInvocation(boost::none);
            } else {
                BSONObjBuilder nextInvocationBuilder;
                for (auto&& elt : cmdObj) {
                    if (elt.fieldNameStringData() == "topologyVersion"_sd) {
                        BSONObjBuilder topologyVersionBuilder(
                            nextInvocationBuilder.subobjStart("topologyVersion"));
                        currentMongosTopologyVersion.serialize(&topologyVersionBuilder);
                    } else {
                        nextInvocationBuilder.append(elt);
                    }
                }
                replyBuilder->setNextInvocation(nextInvocationBuilder.obj());
            }
        }

        return true;
    }

} isMaster;

}  // namespace
}  // namespace mongo
