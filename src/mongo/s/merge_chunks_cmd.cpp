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
    class MergeChunksPassCommand : public Command {
    public:
        MergeChunksPassCommand() : Command("mergeChunks") {}

        virtual void help(stringstream& h) const {
            h << "Merge Chunks command\n"
              << "usage: { mergeChunks : <ns>, bounds : [ <min key>, <max key> ] }";
        }

        virtual Status checkAuthForCommand( ClientBasic* client,
                                            const std::string& dbname,
                                            const BSONObj& cmdObj ) {
            return client->getAuthorizationSession()->checkAuthForPrivilege(
                Privilege( AuthorizationManager::CLUSTER_RESOURCE_NAME,
                           ActionType::mergeChunks ) );
        }

        virtual bool slaveOk() const { return false; }
        virtual LockType locktype() const { return NONE; }

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


        bool run( const string& dbname,
                  BSONObj& cmdObj,
                  int,
                  string& errmsg,
                  BSONObjBuilder& result,
                  bool ) {

            string ns;
            if ( !FieldParser::extract( cmdObj, nsField, &ns, &errmsg ) ) {
                return false;
            }

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

            ShardPtr mergeShard = guessMergeShard( NamespaceString( ns ), minKey );

            if ( !mergeShard ) {
                errmsg = (string)"could not find shard for merge range starting at "
                                 + minKey.toString();
                return false;
            }

            BSONObjBuilder remoteCmdObjB;
            remoteCmdObjB.append( cmdObj[ MergeChunksPassCommand::nsField() ] );
            remoteCmdObjB.append( cmdObj[ MergeChunksPassCommand::boundsField() ] );
            remoteCmdObjB.append( MergeChunksPassCommand::configField(),
                                  configServer.getPrimary().getAddress().toString() );
            remoteCmdObjB.append( MergeChunksPassCommand::shardNameField(),
                                  mergeShard->getName() );

            BSONObj remoteResult;
            // Throws, but handled at level above.  Don't want to rewrap to preserve exception
            // formatting.
            ScopedDbConnection conn( mergeShard->getAddress() );
            bool ok = conn->runCommand( "admin", remoteCmdObjB.obj(), remoteResult );
            conn.done();

            // Always refresh our chunks afterwards
            refreshChunkCache( NamespaceString( ns ) );

            result.appendElements( remoteResult );
            return ok;
        }
    };

    BSONField<string> MergeChunksPassCommand::nsField( "mergeChunks" );
    BSONField<vector<BSONObj> > MergeChunksPassCommand::boundsField( "bounds" );

    BSONField<string> MergeChunksPassCommand::configField( "config" );
    BSONField<string> MergeChunksPassCommand::shardNameField( "shardName" );

    MONGO_INITIALIZER(InitMergeChunksPassCommand)(InitializerContext* context) {
        // Leaked intentionally: a Command registers itself when constructed.
        new MergeChunksPassCommand();
        return Status::OK();
    }
}
