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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/base/init.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/d_logic.h"
#include "mongo/s/d_merge.h"

namespace mongo {

    /**
     * Mongod-side command for merging chunks.
     */
    class MergeChunksCommand : public Command {
    public:
        MergeChunksCommand() : Command("mergeChunks") {}

        virtual void help(stringstream& h) const {
            h << "Merge Chunks command\n"
              << "usage: { mergeChunks : <ns>, bounds : [ <min key>, <max key> ],"
              << " (opt) epoch : <epoch>, (opt) config : <configdb string>,"
              << " (opt) shardName : <shard name> }";
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
        // Optional, if the merge is only valid for a particular epoch
        static BSONField<OID> epochField;
        // Optional, if our sharding state has not previously been initializeed
        static BSONField<string> shardNameField;
        static BSONField<string> configField;

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

            //
            // This might be the first call from mongos, so we may need to pass the config and shard
            // information to initialize the shardingState.
            //

            string config;
            FieldParser::FieldState extracted = FieldParser::extract( cmdObj,
                                                                      configField,
                                                                      &config,
                                                                      &errmsg );
            if ( !extracted ) return false;
            if ( extracted != FieldParser::FIELD_NONE ) {
                ShardingState::initialize( config );
            }
            else if ( !shardingState.enabled() ) {
                errmsg =
                    "sharding state must be enabled or config server specified to merge chunks";
                return false;
            }

            // ShardName is optional, but might not be set yet
            string shardName;
            extracted = FieldParser::extract( cmdObj, shardNameField, &shardName, &errmsg );

            if ( !extracted ) return false;
            if ( extracted != FieldParser::FIELD_NONE ) {
                shardingState.gotShardName( shardName );
            }

            //
            // Epoch is optional, and if not set indicates we should use the latest epoch
            //

            OID epoch;
            if ( !FieldParser::extract( cmdObj, epochField, &epoch, &errmsg ) ) {
                return false;
            }

            return mergeChunks( NamespaceString( ns ), minKey, maxKey, epoch, true, &errmsg );
        }
    };

    BSONField<string> MergeChunksCommand::nsField( "mergeChunks" );
    BSONField<vector<BSONObj> > MergeChunksCommand::boundsField( "bounds" );

    BSONField<string> MergeChunksCommand::configField( "config" );
    BSONField<string> MergeChunksCommand::shardNameField( "shardName" );
    BSONField<OID> MergeChunksCommand::epochField( "epoch" );

    MONGO_INITIALIZER(InitMergeChunksCommand)(InitializerContext* context) {
        // Leaked intentionally: a Command registers itself when constructed.
        new MergeChunksCommand();
        return Status::OK();
    }
}
