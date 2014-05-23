/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/base/init.h"
#include "mongo/client/connpool.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/config.h" // For config server and DBConfig and version refresh
#include "mongo/s/grid.h"
#include "mongo/s/shard.h"

namespace mongo {

    /**
     * Mongos-side command for merging chunks, passes command to appropriate shard.
     */
    class ClusterMergeChunksCommand : public Command {
    public:
        ClusterMergeChunksCommand() : Command("mergeChunks") {}

        virtual void help(stringstream& h) const {
            h << "Merge Chunks command\n"
              << "usage: { mergeChunks : <ns>, bounds : [ <min key>, <max key> ] }";
        }

        virtual Status checkAuthForCommand(OperationContext* txn,
                                           ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            if (!client->getAuthorizationSession()->isAuthorizedForActionsOnResource(
                    txn,
                    ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                    ActionType::splitChunk)) {
                return Status(ErrorCodes::Unauthorized, "Unauthorized");
            }
            return Status::OK();
        }

        virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
            return parseNsFullyQualified(dbname, cmdObj);
        }

        virtual bool adminOnly() const { return true; }
        virtual bool slaveOk() const { return false; }
        virtual bool isWriteCommandForConfigServer() const { return false; }

        // Required
        static BSONField<string> nsField;
        static BSONField<vector<BSONObj> > boundsField;

        // Used to send sharding state
        static BSONField<string> shardNameField;
        static BSONField<string> configField;

        // TODO:  Same limitations as other mongos metadata commands, sometimes we'll be stale here
        // and fail.  Need to better integrate targeting with commands.
        ShardPtr guessMergeShard( const NamespaceString& nss, const BSONObj& minKey ) {

            DBConfigPtr config = grid.getDBConfig( nss.ns() );
            if ( !config->isSharded( nss ) ) {
                config->reload();
                if ( !config->isSharded( nss ) ) {
                    return ShardPtr();
                }
            }

            ChunkManagerPtr manager = config->getChunkManager( nss );
            if ( !manager ) return ShardPtr();
            ChunkPtr chunk = manager->findChunkForDoc( minKey );
            if ( !chunk ) return ShardPtr();
            return ShardPtr( new Shard( chunk->getShard() ) );
        }

        // TODO: This refresh logic should be consolidated
        void refreshChunkCache( const NamespaceString& nss ) {

            DBConfigPtr config = grid.getDBConfig( nss.ns() );
            if ( !config->isSharded( nss ) ) return;

            // Refreshes chunks as a side-effect
            config->getChunkManagerIfExists( nss, true );
        }


        bool run(OperationContext* txn, const string& dbname,
                  BSONObj& cmdObj,
                  int,
                  string& errmsg,
                  BSONObjBuilder& result,
                  bool ) {

            string ns = parseNs(dbname, cmdObj);

            if ( ns.size() == 0 ) {
                errmsg = "no namespace specified";
                return false;
            }

            vector<BSONObj> bounds;
            if ( !FieldParser::extract( cmdObj, boundsField, &bounds, &errmsg ) ) {
                return false;
            }

            if ( bounds.size() == 0 ) {
                errmsg = "no bounds were specified";
                return false;
            }

            if ( bounds.size() != 2 ) {
                errmsg = "only a min and max bound may be specified";
                return false;
            }

            BSONObj minKey = bounds[0];
            BSONObj maxKey = bounds[1];

            if ( minKey.isEmpty() ) {
                errmsg = "no min key specified";
                return false;
            }

            if ( maxKey.isEmpty() ) {
                errmsg = "no max key specified";
                return false;
            }

            // This refreshes the chunk metadata if stale.
            refreshChunkCache( NamespaceString( ns ) );

            ShardPtr mergeShard = guessMergeShard( NamespaceString( ns ), minKey );

            if ( !mergeShard ) {
                errmsg = (string)"could not find shard for merge range starting at "
                                 + minKey.toString();
                return false;
            }

            BSONObjBuilder remoteCmdObjB;
            remoteCmdObjB.append( cmdObj[ ClusterMergeChunksCommand::nsField() ] );
            remoteCmdObjB.append( cmdObj[ ClusterMergeChunksCommand::boundsField() ] );
            remoteCmdObjB.append( ClusterMergeChunksCommand::configField(),
                                  configServer.getPrimary().getAddress().toString() );
            remoteCmdObjB.append( ClusterMergeChunksCommand::shardNameField(),
                                  mergeShard->getName() );

            BSONObj remoteResult;
            // Throws, but handled at level above.  Don't want to rewrap to preserve exception
            // formatting.
            ScopedDbConnection conn( mergeShard->getAddress() );
            bool ok = conn->runCommand( "admin", remoteCmdObjB.obj(), remoteResult );
            conn.done();

            result.appendElements( remoteResult );
            return ok;
        }
    };

    BSONField<string> ClusterMergeChunksCommand::nsField( "mergeChunks" );
    BSONField<vector<BSONObj> > ClusterMergeChunksCommand::boundsField( "bounds" );

    BSONField<string> ClusterMergeChunksCommand::configField( "config" );
    BSONField<string> ClusterMergeChunksCommand::shardNameField( "shardName" );

    MONGO_INITIALIZER(InitMergeChunksPassCommand)(InitializerContext* context) {
        // Leaked intentionally: a Command registers itself when constructed.
        new ClusterMergeChunksCommand();
        return Status::OK();
    }
}
