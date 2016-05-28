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

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/cloner.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/copydb.h"
#include "mongo/db/commands/copydb_start_commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"

namespace {

using namespace mongo;

using std::string;
using std::stringstream;

/* Usage:
 * admindb.$cmd.findOne( { copydb: 1, fromhost: <connection string>, fromdb: <db>,
 *                         todb: <db>[, username: <username>, nonce: <nonce>, key: <key>] } );
 *
 * The "copydb" command is used to copy a database.  Note that this is a very broad definition.
 * This means that the "copydb" command can be used in the following ways:
 *
 * 1. To copy a database within a single node
 * 2. To copy a database within a sharded cluster, possibly to another shard
 * 3. To copy a database from one cluster to another
 *
 * Note that in all cases both the target and source database must be unsharded.
 *
 * The "copydb" command gets sent by the client or the mongos to the destination of the copy
 * operation.  The node, cluster, or shard that recieves the "copydb" command must then query
 * the source of the database to be copied for all the contents and metadata of the database.
 *
 *
 *
 * When used with auth, there are two different considerations.
 *
 * The first is authentication with the target.  The only entity that needs to authenticate with
 * the target node is the client, so authentication works there the same as it would with any
 * other command.
 *
 * The second is the authentication of the target with the source, which is needed because the
 * target must query the source directly for the contents of the database.  To do this, the
 * client must use the "copydbgetnonce" command, in which the target will get a nonce from the
 * source and send it back to the client.  The client can then hash its password with the nonce,
 * send it to the target when it runs the "copydb" command, which can then use that information
 * to authenticate with the source.
 *
 * NOTE: mongos doesn't know how to call or handle the "copydbgetnonce" command.  See
 * SERVER-6427.
 *
 * NOTE: Since internal cluster auth works differently, "copydb" currently doesn't work between
 * shards in a cluster when auth is enabled.  See SERVER-13080.
 */
class CmdCopyDb : public Command {
public:
    CmdCopyDb() : Command("copydb") {}

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        return copydb::checkAuthForCopydbCommand(client, dbname, cmdObj);
    }

    virtual void help(stringstream& help) const {
        help << "copy a database from another host to this host\n";
        help << "usage: {copydb: 1, fromhost: <connection string>, fromdb: <db>, todb: <db>"
             << "[, slaveOk: <bool>, username: <username>, nonce: <nonce>, key: <key>]}";
    }

    virtual bool run(OperationContext* txn,
                     const string& dbname,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        boost::optional<DisableDocumentValidation> maybeDisableValidation;
        if (shouldBypassDocumentValidationForCommand(cmdObj))
            maybeDisableValidation.emplace(txn);

        string fromhost = cmdObj.getStringField("fromhost");
        bool fromSelf = fromhost.empty();
        if (fromSelf) {
            /* copy from self */
            stringstream ss;
            ss << "localhost:" << serverGlobalParams.port;
            fromhost = ss.str();
        }

        CloneOptions cloneOptions;
        cloneOptions.fromDB = cmdObj.getStringField("fromdb");
        cloneOptions.slaveOk = cmdObj["slaveOk"].trueValue();
        cloneOptions.useReplAuth = false;
        cloneOptions.snapshot = true;

        string todb = cmdObj.getStringField("todb");
        if (fromhost.empty() || todb.empty() || cloneOptions.fromDB.empty()) {
            errmsg =
                "params missing - {copydb: 1, fromhost: <connection string>, "
                "fromdb: <db>, todb: <db>}";
            return false;
        }

        if (!NamespaceString::validDBName(todb, NamespaceString::DollarInDbNameBehavior::Allow)) {
            errmsg = "invalid todb name: " + todb;
            return false;
        }

        Cloner cloner;

        // Get MONGODB-CR parameters
        string username = cmdObj.getStringField("username");
        string nonce = cmdObj.getStringField("nonce");
        string key = cmdObj.getStringField("key");

        auto& authConn = CopyDbAuthConnection::forClient(txn->getClient());

        if (!username.empty() && !nonce.empty() && !key.empty()) {
            uassert(13008, "must call copydbgetnonce first", authConn.get());
            BSONObj ret;
            {
                if (!authConn->runCommand(
                        cloneOptions.fromDB,
                        BSON("authenticate" << 1 << "user" << username << "nonce" << nonce << "key"
                                            << key),
                        ret)) {
                    errmsg = "unable to login " + ret.toString();
                    authConn.reset();
                    return false;
                }
            }
            cloner.setConnection(authConn.release());
        } else if (cmdObj.hasField(saslCommandConversationIdFieldName) &&
                   cmdObj.hasField(saslCommandPayloadFieldName)) {
            uassert(25487, "must call copydbsaslstart first", authConn.get());
            BSONObj ret;
            if (!authConn->runCommand(
                    cloneOptions.fromDB,
                    BSON("saslContinue" << 1 << cmdObj[saslCommandConversationIdFieldName]
                                        << cmdObj[saslCommandPayloadFieldName]),
                    ret)) {
                errmsg = "unable to login " + ret.toString();
                authConn.reset();
                return false;
            }

            if (!ret["done"].Bool()) {
                result.appendElements(ret);
                return true;
            }

            result.append("done", true);
            cloner.setConnection(authConn.release());
        } else if (!fromSelf) {
            // If fromSelf leave the cloner's conn empty, it will use a DBDirectClient instead.
            const ConnectionString cs(uassertStatusOK(ConnectionString::parse(fromhost)));

            DBClientBase* conn = cs.connect(errmsg);
            if (!conn) {
                return false;
            }
            cloner.setConnection(conn);
        }

        // Either we didn't need the authConn (if we even had one), or we already moved it
        // into the cloner so just make sure we don't keep it around if we don't need it.
        authConn.reset();

        if (fromSelf) {
            // SERVER-4328 todo lock just the two db's not everything for the fromself case
            ScopedTransaction transaction(txn, MODE_X);
            Lock::GlobalWrite lk(txn->lockState());
            uassertStatusOK(cloner.copyDb(txn, todb, fromhost, cloneOptions, NULL));
        } else {
            ScopedTransaction transaction(txn, MODE_IX);
            Lock::DBLock lk(txn->lockState(), todb, MODE_X);
            uassertStatusOK(cloner.copyDb(txn, todb, fromhost, cloneOptions, NULL));
        }

        return true;
    }

} cmdCopyDB;

}  // namespace
