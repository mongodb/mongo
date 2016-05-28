/**
*    Copyright (C) 2008 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/commands/copydb_start_commands.h"

#include "mongo/base/status.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/client.h"
#include "mongo/db/cloner.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/copydb.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;
using std::stringstream;

namespace {
const auto authConnection = Client::declareDecoration<std::unique_ptr<DBClientBase>>();
}  // namespace

std::unique_ptr<DBClientBase>& CopyDbAuthConnection::forClient(Client* client) {
    return authConnection(client);
}

/* Usage:
 * admindb.$cmd.findOne( { copydbgetnonce: 1, fromhost: <connection string> } );
 *
 * Run against the mongod that is the intended target for the "copydb" command.  Used to get a
 * nonce from the source of a "copydb" operation for authentication purposes.  See the
 * description of the "copydb" command below.
 */
class CmdCopyDbGetNonce : public Command {
public:
    CmdCopyDbGetNonce() : Command("copydbgetnonce") {}

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool slaveOk() const {
        return false;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        // No auth required
    }

    virtual void help(stringstream& help) const {
        help << "get a nonce for subsequent copy db request from secure server\n";
        help << "usage: {copydbgetnonce: 1, fromhost: <hostname>}";
    }

    virtual bool run(OperationContext* txn,
                     const string&,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        string fromhost = cmdObj.getStringField("fromhost");
        if (fromhost.empty()) {
            /* copy from self */
            stringstream ss;
            ss << "localhost:" << serverGlobalParams.port;
            fromhost = ss.str();
        }

        const ConnectionString cs(uassertStatusOK(ConnectionString::parse(fromhost)));

        auto& authConn = CopyDbAuthConnection::forClient(txn->getClient());
        authConn.reset(cs.connect(errmsg));
        if (!authConn) {
            return false;
        }

        BSONObj ret;

        if (!authConn->runCommand("admin", BSON("getnonce" << 1), ret)) {
            errmsg = "couldn't get nonce " + ret.toString();
            authConn.reset();
            return false;
        }

        result.appendElements(ret);
        return true;
    }

} cmdCopyDBGetNonce;

/* Usage:
 * admindb.$cmd.findOne( { copydbsaslstart: 1,
 *                         fromhost: <connection string>,
 *                         mechanism: <String>,
 *                         payload: <BinaryOrString> } );
 *
 * Run against the mongod that is the intended target for the "copydb" command.  Used to
 * initialize a SASL auth session for a "copydb" operation for authentication purposes.
 */
class CmdCopyDbSaslStart : public Command {
public:
    CmdCopyDbSaslStart() : Command("copydbsaslstart") {}

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool slaveOk() const {
        return false;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        // No auth required
        return Status::OK();
    }

    virtual void help(stringstream& help) const {
        help << "Initialize a SASL auth session for subsequent copy db request "
                "from secure server\n";
    }

    virtual bool run(OperationContext* txn,
                     const string&,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        const string fromDb = cmdObj.getStringField("fromdb");

        string fromHost = cmdObj.getStringField("fromhost");
        if (fromHost.empty()) {
            /* copy from self */
            stringstream ss;
            ss << "localhost:" << serverGlobalParams.port;
            fromHost = ss.str();
        }

        const ConnectionString cs(uassertStatusOK(ConnectionString::parse(fromHost)));

        BSONElement mechanismElement;
        Status status = bsonExtractField(cmdObj, saslCommandMechanismFieldName, &mechanismElement);
        if (!status.isOK()) {
            return appendCommandStatus(result, status);
        }

        BSONElement payloadElement;
        status = bsonExtractField(cmdObj, saslCommandPayloadFieldName, &payloadElement);
        if (!status.isOK()) {
            log() << "Failed to extract payload: " << status;
            return false;
        }

        auto& authConn = CopyDbAuthConnection::forClient(txn->getClient());
        authConn.reset(cs.connect(errmsg));
        if (!authConn.get()) {
            return false;
        }

        BSONObj ret;
        if (!authConn->runCommand(
                fromDb, BSON("saslStart" << 1 << mechanismElement << payloadElement), ret)) {
            authConn.reset();
            return appendCommandStatus(result, getStatusFromCommandResult(ret));
        }

        result.appendElements(ret);
        return true;
    }

} cmdCopyDBSaslStart;

}  // namespace mongo
