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
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/metadata/client_metadata_ismaster.h"

namespace mongo {
namespace {

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
                     const std::string&,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {

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

        result.appendBool("ismaster", true);

        result.appendNumber("maxBsonObjectSize", BSONObjMaxUserSize);
        result.appendNumber("maxMessageSizeBytes", MaxMessageSizeBytes);
        result.appendNumber("maxWriteBatchSize", write_ops::kMaxWriteBatchSize);
        result.appendDate("localTime", jsTime());

        result.append("minWireVersion", WireSpec::instance().incomingExternalClient.minWireVersion);
        result.append("maxWireVersion", WireSpec::instance().incomingExternalClient.maxWireVersion);

        result.append("readOnly", storageGlobalParams.readOnly);

        return true;
    }
} CmdIsMaster;

}  // namespace repl
}  // namespace mongo
