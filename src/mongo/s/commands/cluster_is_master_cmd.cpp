/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/wire_version.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/metadata/client_metadata_ismaster.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/util/map_util.h"

namespace mongo {
namespace {

class CmdIsMaster : public BasicCommand {
public:
    CmdIsMaster() : BasicCommand("isMaster", "ismaster") {}

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool slaveOk() const override {
        return true;
    }

    void help(std::stringstream& help) const override {
        help << "test if this is master half of a replica pair";
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) override {
        // No auth required
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto& clientMetadataIsMasterState = ClientMetadataIsMasterState::get(opCtx->getClient());
        bool seenIsMaster = clientMetadataIsMasterState.hasSeenIsMaster();
        if (!seenIsMaster) {
            clientMetadataIsMasterState.setSeenIsMaster();
        }

        BSONElement element = cmdObj[kMetadataDocumentName];
        if (!element.eoo()) {
            if (seenIsMaster) {
                return Command::appendCommandStatus(
                    result,
                    Status(ErrorCodes::ClientMetadataCannotBeMutated,
                           "The client metadata document may only be sent in the first isMaster"));
            }

            auto swParseClientMetadata = ClientMetadata::parse(element);

            if (!swParseClientMetadata.getStatus().isOK()) {
                return Command::appendCommandStatus(result, swParseClientMetadata.getStatus());
            }

            invariant(swParseClientMetadata.getValue());

            swParseClientMetadata.getValue().get().logClientMetadata(opCtx->getClient());

            clientMetadataIsMasterState.setClientMetadata(
                opCtx->getClient(), std::move(swParseClientMetadata.getValue()));
        }

        result.appendBool("ismaster", true);
        result.append("msg", "isdbgrid");
        result.appendNumber("maxBsonObjectSize", BSONObjMaxUserSize);
        result.appendNumber("maxMessageSizeBytes", MaxMessageSizeBytes);
        result.appendNumber("maxWriteBatchSize", write_ops::kMaxWriteBatchSize);
        result.appendDate("localTime", jsTime());

        // Mongos tries to keep exactly the same version range of the server for which
        // it is compiled.
        result.append("maxWireVersion", WireSpec::instance().incoming.maxWireVersion);
        result.append("minWireVersion", WireSpec::instance().incoming.minWireVersion);

        const auto parameter = mapFindWithDefault(ServerParameterSet::getGlobal()->getMap(),
                                                  "automationServiceDescriptor",
                                                  static_cast<ServerParameter*>(nullptr));
        if (parameter)
            parameter->append(opCtx, result, "automationServiceDescriptor");

        MessageCompressorManager::forSession(opCtx->getClient()->session())
            .serverNegotiate(cmdObj, &result);

        return true;
    }

} isMaster;

}  // namespace
}  // namespace mongo
