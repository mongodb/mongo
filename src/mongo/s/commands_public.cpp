// s/commands_public.cpp

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
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#include "mongo/pch.h"

#include "mongo/base/init.h"
#include "mongo/client/connpool.h"
#include "mongo/client/parallel.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands/find_and_modify.h"
#include "mongo/db/commands/mr.h"
#include "mongo/db/commands/rename_collection.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/queryutil.h"
#include "mongo/s/client_info.h"
#include "mongo/s/chunk.h"
#include "mongo/s/config.h"
#include "mongo/s/cursors.h"
#include "mongo/s/grid.h"
#include "mongo/s/interrupt_status_mongos.h"
#include "mongo/s/strategy.h"
#include "mongo/s/version_manager.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/net/message.h"
#include "mongo/util/timer.h"

namespace mongo {

    namespace dbgrid_pub_cmds {

        class PublicGridCommand : public Command {
        public:
            PublicGridCommand( const char* n, const char* oldname=NULL ) : Command( n, false, oldname ) {
            }
            virtual bool slaveOk() const {
                return true;
            }
            virtual bool adminOnly() const {
                return false;
            }

            // Override if passthrough should also send query options
            // Safer as off by default, can slowly enable as we add more tests
            virtual bool passOptions() const { return false; }

            // all grid commands are designed not to lock
            virtual LockType locktype() const { return NONE; }

        protected:

            bool passthrough( DBConfigPtr conf, const BSONObj& cmdObj , BSONObjBuilder& result ) {
                return _passthrough(conf->getName(), conf, cmdObj, 0, result);
            }
            bool adminPassthrough( DBConfigPtr conf, const BSONObj& cmdObj , BSONObjBuilder& result ) {
                return _passthrough("admin", conf, cmdObj, 0, result);
            }

            bool passthrough( DBConfigPtr conf, const BSONObj& cmdObj , int options, BSONObjBuilder& result ) {
                return _passthrough(conf->getName(), conf, cmdObj, options, result);
            }
            bool adminPassthrough( DBConfigPtr conf, const BSONObj& cmdObj , int options, BSONObjBuilder& result ) {
                return _passthrough("admin", conf, cmdObj, options, result);
            }

        private:
            bool _passthrough(const string& db,  DBConfigPtr conf, const BSONObj& cmdObj , int options , BSONObjBuilder& result ) {
                ShardConnection conn( conf->getPrimary() , "" );
                BSONObj res;
                bool ok = conn->runCommand( db , cmdObj , res , passOptions() ? options : 0 );
                if ( ! ok && res["code"].numberInt() == SendStaleConfigCode ) {
                    conn.done();
                    throw RecvStaleConfigException( "command failed because of stale config", res );
                }
                result.appendElements( res );
                conn.done();
                return ok;
            }
        };

        class RunOnAllShardsCommand : public Command {
        public:
            RunOnAllShardsCommand(const char* n, const char* oldname=NULL) : Command(n, false, oldname) {}

            virtual bool slaveOk() const { return true; }
            virtual bool adminOnly() const { return false; }

            // all grid commands are designed not to lock
            virtual LockType locktype() const { return NONE; }


            // default impl uses all shards for DB
            virtual void getShards(const string& dbName , BSONObj& cmdObj, set<Shard>& shards) {
                DBConfigPtr conf = grid.getDBConfig( dbName , false );
                conf->getAllShards(shards);
            }

            virtual void aggregateResults(const vector<BSONObj>& results, BSONObjBuilder& output) {}

            // don't override
            virtual bool run(const string& dbName , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& output, bool) {
                LOG(1) << "RunOnAllShardsCommand db: " << dbName << " cmd:" << cmdObj << endl;
                set<Shard> shards;
                getShards(dbName, cmdObj, shards);

                // TODO: Future is deprecated, replace with commandOp()

                list< shared_ptr<Future::CommandResult> > futures;
                for ( set<Shard>::const_iterator i=shards.begin(), end=shards.end() ; i != end ; i++ ) {
                    futures.push_back( Future::spawnCommand( i->getConnString() , dbName , cmdObj, 0 ) );
                }

                vector<BSONObj> results;
                BSONObjBuilder subobj (output.subobjStart("raw"));
                BSONObjBuilder errors;
                for ( list< shared_ptr<Future::CommandResult> >::iterator i=futures.begin(); i!=futures.end(); i++ ) {
                    shared_ptr<Future::CommandResult> res = *i;
                    if ( ! res->join() ) {

                        BSONObj result = res->result();

                        if( ! result["errmsg"].eoo() ){
                            errors.appendAs(res->result()["errmsg"], res->getServer());
                        }
                        else {

                            // Can happen if message is empty, for some reason
                            errors.append( res->getServer(), str::stream()
                                << "result without error message returned : " << result );
                        }
                    }
                    results.push_back( res->result() );
                    subobj.append( res->getServer() , res->result() );
                }

                subobj.done();

                BSONObj errobj = errors.done();
                if (! errobj.isEmpty()) {
                    errmsg = errobj.toString(false, true);
                    return false;
                }

                aggregateResults(results, output);
                return true;
            }

        };

        class AllShardsCollectionCommand : public RunOnAllShardsCommand {
        public:
            AllShardsCollectionCommand(const char* n, const char* oldname=NULL) : RunOnAllShardsCommand(n, oldname) {}

            virtual void getShards(const string& dbName , BSONObj& cmdObj, set<Shard>& shards) {
                string fullns = dbName + '.' + cmdObj.firstElement().valuestrsafe();

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    shards.insert(conf->getShard(fullns));
                }
                else {
                    conf->getChunkManager(fullns)->getAllShards(shards);
                }
            }
        };


        class NotAllowedOnShardedCollectionCmd : public PublicGridCommand {
        public:
            NotAllowedOnShardedCollectionCmd( const char * n ) : PublicGridCommand( n ) {}

            virtual string getFullNS( const string& dbName , const BSONObj& cmdObj ) = 0;

            virtual bool run(const string& dbName , BSONObj& cmdObj, int options, string& errmsg, BSONObjBuilder& result, bool) {
                string fullns = getFullNS( dbName , cmdObj );

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    return passthrough( conf , cmdObj , options, result );
                }
                errmsg = "can't do command: " + name + " on sharded collection";
                return false;
            }
        };

        // ----

        class DropIndexesCmd : public AllShardsCollectionCommand {
        public:
            DropIndexesCmd() :  AllShardsCollectionCommand("dropIndexes", "deleteIndexes") {}
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::dropIndexes);
                out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
            }
        } dropIndexesCmd;

        class ReIndexCmd : public AllShardsCollectionCommand {
        public:
            ReIndexCmd() :  AllShardsCollectionCommand("reIndex") {}
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::reIndex);
                out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
            }
        } reIndexCmd;

        class CollectionModCmd : public AllShardsCollectionCommand {
        public:
            CollectionModCmd() :  AllShardsCollectionCommand("collMod") {}
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::collMod);
                out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
            }
        } collectionModCmd;

        class ProfileCmd : public PublicGridCommand {
        public:
            ProfileCmd() :  PublicGridCommand("profile") {}
            virtual bool run(const string& dbName , BSONObj& cmdObj, int options, string& errmsg, BSONObjBuilder& result, bool) {
                errmsg = "profile currently not supported via mongos";
                return false;
            }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::profileEnable);
                out->push_back(Privilege(dbname, actions));
            }
        } profileCmd;
        

        class ValidateCmd : public AllShardsCollectionCommand {
        public:
            ValidateCmd() :  AllShardsCollectionCommand("validate") {}
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::validate);
                // TODO: should the resource needed be the collection name instead of the db name?
                out->push_back(Privilege(dbname, actions));
            }
            virtual void aggregateResults(const vector<BSONObj>& results, BSONObjBuilder& output) {
                for (vector<BSONObj>::const_iterator it(results.begin()), end(results.end()); it!=end; it++){
                    const BSONObj& result = *it;
                    const BSONElement valid = result["valid"];
                    if (!valid.eoo()){
                        if (!valid.trueValue()) {
                            output.appendBool("valid", false);
                            return;
                        }
                    }
                    else {
                        // Support pre-1.9.0 output with everything in a big string
                        const char* s = result["result"].valuestrsafe();
                        if (strstr(s, "exception") ||  strstr(s, "corrupt")){
                            output.appendBool("valid", false);
                            return;
                        }
                    }
                }

                output.appendBool("valid", true);
            }
        } validateCmd;

        class RepairDatabaseCmd : public RunOnAllShardsCommand {
        public:
            RepairDatabaseCmd() :  RunOnAllShardsCommand("repairDatabase") {}
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::repairDatabase);
                out->push_back(Privilege(dbname, actions));
            }
        } repairDatabaseCmd;

        class DBStatsCmd : public RunOnAllShardsCommand {
        public:
            DBStatsCmd() :  RunOnAllShardsCommand("dbStats", "dbstats") {}
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::dbStats);
                out->push_back(Privilege(dbname, actions));
            }

            virtual void aggregateResults(const vector<BSONObj>& results, BSONObjBuilder& output) {
                long long objects = 0;
                long long unscaledDataSize = 0;
                long long dataSize = 0;
                long long storageSize = 0;
                long long numExtents = 0;
                long long indexes = 0;
                long long indexSize = 0;
                long long fileSize = 0;

                for (vector<BSONObj>::const_iterator it(results.begin()), end(results.end()); it != end; ++it) {
                    const BSONObj& b = *it;
                    objects     += b["objects"].numberLong();
                    unscaledDataSize    += b["avgObjSize"].numberLong() * b["objects"].numberLong();
                    dataSize    += b["dataSize"].numberLong();
                    storageSize += b["storageSize"].numberLong();
                    numExtents  += b["numExtents"].numberLong();
                    indexes     += b["indexes"].numberLong();
                    indexSize   += b["indexSize"].numberLong();
                    fileSize    += b["fileSize"].numberLong();
                }

                //result.appendNumber( "collections" , ncollections ); //TODO: need to find a good way to get this
                output.appendNumber( "objects" , objects );
                /* avgObjSize on mongod is not scaled based on the argument to db.stats(), so we use
                 * unscaledDataSize here for consistency.  See SERVER-7347. */
                output.append      ( "avgObjSize" , objects == 0 ? 0 : double(unscaledDataSize) /
                                                                       double(objects) );
                output.appendNumber( "dataSize" , dataSize );
                output.appendNumber( "storageSize" , storageSize);
                output.appendNumber( "numExtents" , numExtents );
                output.appendNumber( "indexes" , indexes );
                output.appendNumber( "indexSize" , indexSize );
                output.appendNumber( "fileSize" , fileSize );
            }
        } DBStatsCmdObj;

        class CreateCmd : public PublicGridCommand {
        public:
            CreateCmd() : PublicGridCommand( "create" ) {}
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::createCollection);
                out->push_back(Privilege(dbname, actions));
            }
            bool run(const string& dbName,
                     BSONObj& cmdObj,
                     int,
                     string&,
                     BSONObjBuilder& result,
                     bool) {
                DBConfigPtr conf = grid.getDBConfig( dbName , false );
                return passthrough( conf , cmdObj , result );
            }
        } createCmd;

        class DropCmd : public PublicGridCommand {
        public:
            DropCmd() : PublicGridCommand( "drop" ) {}
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::dropCollection);
                out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
            }
            bool run(const string& dbName , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                log() << "DROP: " << fullns << endl;

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    log() << "\tdrop going to do passthrough" << endl;
                    return passthrough( conf , cmdObj , result );
                }

                //
                // TODO: There will be problems if we simultaneously shard and drop a collection
                //

                ChunkManagerPtr cm;
                ShardPtr primary;
                conf->getChunkManagerOrPrimary( fullns, cm, primary );

                if( ! cm ) {
                    log() << "\tdrop going to do passthrough after re-check" << endl;
                    return passthrough( conf , cmdObj , result );
                }

                cm->drop( cm );

                if( ! conf->removeSharding( fullns ) ){
                    warning() << "collection " << fullns
                              << " was reloaded as unsharded before drop completed"
                              << " during single drop" << endl;
                }

                return 1;
            }
        } dropCmd;

        class DropDBCmd : public PublicGridCommand {
        public:
            DropDBCmd() : PublicGridCommand( "dropDatabase" ) {}
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::dropDatabase);
                out->push_back(Privilege(dbname, actions));
            }
            bool run(const string& dbName , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                // disallow dropping the config database from mongos
                if( dbName == "config" ) {
                    errmsg = "Cannot drop 'config' database via mongos";
                    return false;
                }

                BSONElement e = cmdObj.firstElement();

                if ( ! e.isNumber() || e.number() != 1 ) {
                    errmsg = "invalid params";
                    return 0;
                }

                DBConfigPtr conf = grid.getDBConfig( dbName, false );

                log() << "DROP DATABASE: " << dbName << endl;

                if ( ! conf ) {
                    result.append( "info" , "database didn't exist" );
                    return true;
                }

                //
                // Reload the database configuration so that we're sure a database entry exists
                // TODO: This won't work with parallel dropping
                //

                grid.removeDBIfExists( *conf );
                grid.getDBConfig( dbName );

                // TODO: Make dropping logic saner and more tolerant of partial drops.  This is
                // particularly important since a database drop can be aborted by *any* collection
                // with a distributed namespace lock taken (migrates/splits)

                //
                // Create a copy of the DB config object to drop, so that no one sees a weird
                // intermediate version of the info
                //

                DBConfig confCopy( conf->getName() );
                if( ! confCopy.load() ){
                    errmsg = "could not load database info to drop";
                    return false;
                }

                // Enable sharding so we can correctly retry failed drops
                // This will re-drop old sharded entries if they exist
                confCopy.enableSharding( false );

                if ( ! confCopy.dropDatabase( errmsg ) )
                    return false;

                result.append( "dropped" , dbName );
                return true;
            }
        } dropDBCmd;

        class RenameCollectionCmd : public PublicGridCommand {
        public:
            RenameCollectionCmd() : PublicGridCommand( "renameCollection" ) {}
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                rename_collection::addPrivilegesRequiredForRenameCollection(cmdObj, out);
            }
            bool run(const string& dbName, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string fullnsFrom = cmdObj.firstElement().valuestrsafe();
                string dbNameFrom = nsToDatabase( fullnsFrom );
                DBConfigPtr confFrom = grid.getDBConfig( dbNameFrom , false );

                string fullnsTo = cmdObj["to"].valuestrsafe();
                string dbNameTo = nsToDatabase( fullnsTo );
                DBConfigPtr confTo = grid.getDBConfig( dbNameTo , false );

                uassert(13140, "Don't recognize source or target DB", confFrom && confTo);
                uassert(13138, "You can't rename a sharded collection", !confFrom->isSharded(fullnsFrom));
                uassert(13139, "You can't rename to a sharded collection", !confTo->isSharded(fullnsTo));

                const Shard& shardTo = confTo->getShard(fullnsTo);
                const Shard& shardFrom = confFrom->getShard(fullnsFrom);

                uassert(13137, "Source and destination collections must be on same shard", shardFrom == shardTo);

                return adminPassthrough( confFrom , cmdObj , result );
            }
        } renameCollectionCmd;

        class CopyDBCmd : public PublicGridCommand {
        public:
            CopyDBCmd() : PublicGridCommand( "copydb" ) {}
            virtual bool adminOnly() const {
                return true;
            }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                // Note: privileges required are currently only granted to old-style users for
                // backwards compatibility, since we can't properly handle auth checking for the
                // read from the source DB.
                ActionSet actions;
                actions.addAction(ActionType::copyDBTarget);
                out->push_back(Privilege(dbname, actions)); // NOTE: dbname is always admin
            }
            bool run(const string& dbName, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string todb = cmdObj.getStringField("todb");
                uassert(13402, "need a todb argument", !todb.empty());

                DBConfigPtr confTo = grid.getDBConfig( todb );
                uassert(13398, "cant copy to sharded DB", !confTo->isShardingEnabled());

                string fromhost = cmdObj.getStringField("fromhost");
                if (!fromhost.empty()) {
                    return adminPassthrough( confTo , cmdObj , result );
                }
                else {
                    string fromdb = cmdObj.getStringField("fromdb");
                    uassert(13399, "need a fromdb argument", !fromdb.empty());

                    DBConfigPtr confFrom = grid.getDBConfig( fromdb , false );
                    uassert(13400, "don't know where source DB is", confFrom);
                    uassert(13401, "cant copy from sharded DB", !confFrom->isShardingEnabled());

                    BSONObjBuilder b;
                    BSONForEach(e, cmdObj) {
                        if (strcmp(e.fieldName(), "fromhost") != 0)
                            b.append(e);
                    }
                    b.append("fromhost", confFrom->getPrimary().getConnString());
                    BSONObj fixed = b.obj();

                    return adminPassthrough( confTo , fixed , result );
                }

            }
        };
        MONGO_INITIALIZER(RegisterCopyDBCommand)(InitializerContext* context) {
            // Leaked intentionally: a Command registers itself when constructed.
            // NOTE: this initializer block cannot be removed due to SERVER-9167
            new CopyDBCmd();
            return Status::OK();
        }

        class CountCmd : public PublicGridCommand {
        public:
            CountCmd() : PublicGridCommand( "count" ) { }
            virtual bool passOptions() const { return true; }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::find);
                out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
            }
            bool run( const string& dbName,
                    BSONObj& cmdObj,
                    int options,
                    string& errmsg,
                    BSONObjBuilder& result,
                    bool ){

                long long skip = 0;
                if( cmdObj["skip"].isNumber() ){
                    skip = cmdObj["skip"].numberLong();
                    if( skip < 0 ){
                        errmsg = "skip value is negative in count query";
                        return false;
                    }
                }
                else if( cmdObj["skip"].ok() ){
                    errmsg = "skip value is not a valid number";
                    return false;
                }

                const string collection = cmdObj.firstElement().valuestrsafe();
                const string fullns = dbName + "." + collection;

                BSONObjBuilder countCmdBuilder;
                countCmdBuilder.append( "count", collection );

                BSONObj filter;
                if( cmdObj["query"].isABSONObj() ){
                    countCmdBuilder.append( "query", cmdObj["query"].Obj() );
                    filter = cmdObj["query"].Obj();
                }

                if( cmdObj["limit"].isNumber() ){
                    long long limit = cmdObj["limit"].numberLong();

                    /* We only need to factor in the skip value when sending to
                     * the shards if we have a value for limit, otherwise, we
                     * apply it only once we have collected all counts.
                     */
                    if( limit != 0 && cmdObj["skip"].isNumber() ){
                        if ( limit > 0 )
                            limit += skip;
                        else
                            limit -= skip;
                    }

                    countCmdBuilder.append( "limit", limit );
                }

                if (cmdObj.hasField("$queryOptions")) {
                    countCmdBuilder.append(cmdObj["$queryOptions"]);
                }

                vector<Strategy::CommandResult> countResult;

                SHARDED->commandOp( dbName, countCmdBuilder.done(),
                            options, fullns, filter, &countResult );

                long long total = 0;
                BSONObjBuilder shardSubTotal( result.subobjStart( "shards" ));

                for( vector<Strategy::CommandResult>::const_iterator iter = countResult.begin();
                        iter != countResult.end(); ++iter ){
                    const string& shardName = iter->shardTarget.getName();

                    if( iter->result["ok"].trueValue() ){
                        long long shardCount = iter->result["n"].numberLong();

                        shardSubTotal.appendNumber( shardName, shardCount );
                        total += shardCount;
                    }
                    else {
                        shardSubTotal.doneFast();
                        errmsg = "failed on : " + shardName;
                        result.append( "cause", iter->result );
                        return false;
                    }
                }

                shardSubTotal.doneFast();
                total = applySkipLimit( total , cmdObj );
                result.appendNumber( "n" , total );

                return true;
            }
        } countCmd;

        class CollectionStats : public PublicGridCommand {
        public:
            CollectionStats() : PublicGridCommand("collStats", "collstats") { }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::collStats);
                out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
            }
            bool run(const string& dbName , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    result.appendBool("sharded", false);
                    result.append( "primary" , conf->getPrimary().getName() );
                    return passthrough( conf , cmdObj , result);
                }
                result.appendBool("sharded", true);

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 12594 ,  "how could chunk manager be null!" , cm );

                set<Shard> servers;
                cm->getAllShards(servers);

                BSONObjBuilder shardStats;
                map<string,long long> counts;
                map<string,long long> indexSizes;
                /*
                long long count=0;
                long long size=0;
                long long storageSize=0;
                */
                int nindexes=0;
                bool warnedAboutIndexes = false;
                for ( set<Shard>::iterator i=servers.begin(); i!=servers.end(); i++ ) {
                    BSONObj res;
                    {
                        ScopedDbConnection conn(i->getConnString());
                        if ( ! conn->runCommand( dbName , cmdObj , res ) ) {
                            errmsg = "failed on shard: " + res.toString();
                            return false;
                        }
                        conn.done();
                    }
                    
                    BSONObjIterator j( res );
                    while ( j.more() ) {
                        BSONElement e = j.next();

                        if ( str::equals( e.fieldName() , "ns" ) || 
                             str::equals( e.fieldName() , "ok" ) || 
                             str::equals( e.fieldName() , "avgObjSize" ) ||
                             str::equals( e.fieldName() , "lastExtentSize" ) ||
                             str::equals( e.fieldName() , "paddingFactor" ) ) {
                            continue;
                        }
                        else if ( str::equals( e.fieldName() , "count" ) ||
                                  str::equals( e.fieldName() , "size" ) ||
                                  str::equals( e.fieldName() , "storageSize" ) ||
                                  str::equals( e.fieldName() , "numExtents" ) ||
                                  str::equals( e.fieldName() , "totalIndexSize" ) ) {
                            counts[e.fieldName()] += e.numberLong();
                        }
                        else if ( str::equals( e.fieldName() , "indexSizes" ) ) {
                            BSONObjIterator k( e.Obj() );
                            while ( k.more() ) {
                                BSONElement temp = k.next();
                                indexSizes[temp.fieldName()] += temp.numberLong();
                            }
                        }
                        else if ( str::equals( e.fieldName() , "flags" ) ) {
                            if ( ! result.hasField( e.fieldName() ) )
                                result.append( e );
                        }
                        else if ( str::equals( e.fieldName() , "nindexes" ) ) {
                            int myIndexes = e.numberInt();
                            
                            if ( nindexes == 0 ) {
                                nindexes = myIndexes;
                            }
                            else if ( nindexes == myIndexes ) {
                                // no-op
                            }
                            else {
                                // hopefully this means we're building an index
                                
                                if ( myIndexes > nindexes )
                                    nindexes = myIndexes;
                                
                                if ( ! warnedAboutIndexes ) {
                                    result.append( "warning" , "indexes don't all match - ok if ensureIndex is running" );
                                    warnedAboutIndexes = true;
                                }
                            }
                        }
                        else {
                            warning() << "mongos collstats doesn't know about: " << e.fieldName() << endl;
                        }
                        
                    }
                    shardStats.append(i->getName(), res);
                }

                result.append("ns", fullns);
                
                for ( map<string,long long>::iterator i=counts.begin(); i!=counts.end(); ++i )
                    result.appendNumber( i->first , i->second );
                
                {
                    BSONObjBuilder ib( result.subobjStart( "indexSizes" ) );
                    for ( map<string,long long>::iterator i=indexSizes.begin(); i!=indexSizes.end(); ++i )
                        ib.appendNumber( i->first , i->second );
                    ib.done();
                }

                if ( counts["count"] > 0 )
                    result.append("avgObjSize", (double)counts["size"] / (double)counts["count"] );
                else
                    result.append( "avgObjSize", 0.0 );
                
                result.append("nindexes", nindexes);

                result.append("nchunks", cm->numChunks());
                result.append("shards", shardStats.obj());

                return true;
            }
        } collectionStatsCmd;

        class FindAndModifyCmd : public PublicGridCommand {
        public:
            FindAndModifyCmd() : PublicGridCommand("findAndModify", "findandmodify") { }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                find_and_modify::addPrivilegesRequiredForFindAndModify(dbname, cmdObj, out);
            }
            bool run(const string& dbName, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    return passthrough( conf , cmdObj , result);
                }

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 13002 ,  "shard internal error chunk manager should never be null" , cm );

                BSONObj filter = cmdObj.getObjectField("query");
                uassert(13343,  "query for sharded findAndModify must have shardkey", cm->hasShardKey(filter));

                ChunkPtr chunk = cm->findChunkForDoc(filter);
                ShardConnection conn( chunk->getShard() , fullns );
                BSONObj res;
                bool ok = conn->runCommand( conf->getName() , cmdObj , res );
                conn.done();

                if (!ok && res.getIntField("code") == RecvStaleConfigCode) { // code for RecvStaleConfigException
                    throw RecvStaleConfigException( "FindAndModify", res ); // Command code traps this and re-runs
                }

                if (ok) {
                    // check whether split is necessary (using update object for size heuristic)
                    ClientInfo *client = ClientInfo::get();
                        
                    if (client != NULL && client->autoSplitOk()) {
                      chunk->splitIfShould( cmdObj.getObjectField("update").objsize() ); 
                    }
                }

                result.appendElements(res);
                return ok;
            }

        } findAndModifyCmd;

        class DataSizeCmd : public PublicGridCommand {
        public:
            DataSizeCmd() : PublicGridCommand("dataSize", "datasize") { }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::find);
                out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
            }
            bool run(const string& dbName, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string fullns = cmdObj.firstElement().String();

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    return passthrough( conf , cmdObj , result);
                }

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 13407 ,  "how could chunk manager be null!" , cm );

                BSONObj min = cmdObj.getObjectField( "min" );
                BSONObj max = cmdObj.getObjectField( "max" );
                BSONObj keyPattern = cmdObj.getObjectField( "keyPattern" );

                uassert( 13408, "keyPattern must equal shard key",
                         cm->getShardKey().key() == keyPattern );
                uassert( 13405, str::stream() << "min value " << min << " does not have shard key",
                         cm->hasShardKey(min) );
                uassert( 13406, str::stream() << "max value " << max << " does not have shard key",
                         cm->hasShardKey(max) );

                // yes these are doubles...
                double size = 0;
                double numObjects = 0;
                int millis = 0;

                set<Shard> shards;
                cm->getShardsForRange(shards, min, max);
                for ( set<Shard>::iterator i=shards.begin(), end=shards.end() ; i != end; ++i ) {
                    ScopedDbConnection conn(i->getConnString());
                    BSONObj res;
                    bool ok = conn->runCommand( conf->getName() , cmdObj , res );
                    conn.done();

                    if ( ! ok ) {
                        result.appendElements( res );
                        return false;
                    }

                    size       += res["size"].number();
                    numObjects += res["numObjects"].number();
                    millis     += res["millis"].numberInt();

                }

                result.append( "size", size );
                result.append( "numObjects" , numObjects );
                result.append( "millis" , millis );
                return true;
            }

        } DataSizeCmd;

        class ConvertToCappedCmd : public NotAllowedOnShardedCollectionCmd  {
        public:
            ConvertToCappedCmd() : NotAllowedOnShardedCollectionCmd("convertToCapped") {}
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::convertToCapped);
                out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
            }

            virtual string getFullNS( const string& dbName , const BSONObj& cmdObj ) {
                return dbName + "." + cmdObj.firstElement().valuestrsafe();
            }

        } convertToCappedCmd;


        class GroupCmd : public NotAllowedOnShardedCollectionCmd  {
        public:
            GroupCmd() : NotAllowedOnShardedCollectionCmd("group") {}
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::find);
                out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
            }
            virtual bool passOptions() const { return true; }
            virtual string getFullNS( const string& dbName , const BSONObj& cmdObj ) {
                return dbName + "." + cmdObj.firstElement().embeddedObjectUserCheck()["ns"].valuestrsafe();
            }

        } groupCmd;

        class SplitVectorCmd : public NotAllowedOnShardedCollectionCmd  {
        public:
            SplitVectorCmd() : NotAllowedOnShardedCollectionCmd("splitVector") {}
            virtual bool passOptions() const { return true; }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::splitVector);
                out->push_back(Privilege(AuthorizationManager::CLUSTER_RESOURCE_NAME, actions));
            }
            virtual bool run(const string& dbName , BSONObj& cmdObj, int options, string& errmsg, BSONObjBuilder& result, bool) {
                string x = cmdObj.firstElement().valuestrsafe();
                if ( ! str::startsWith( x , dbName ) ) {
                    errmsg = str::stream() << "doing a splitVector across dbs isn't supported via mongos";
                    return false;
                }
                return NotAllowedOnShardedCollectionCmd::run( dbName , cmdObj , options , errmsg, result, false );
            }
            virtual string getFullNS( const string& dbName , const BSONObj& cmdObj ) {
                return cmdObj.firstElement().valuestrsafe();
            }

        } splitVectorCmd;


        class DistinctCmd : public PublicGridCommand {
        public:
            DistinctCmd() : PublicGridCommand("distinct") {}
            virtual void help( stringstream &help ) const {
                help << "{ distinct : 'collection name' , key : 'a.b' , query : {} }";
            }
            virtual bool passOptions() const { return true; }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::find);
                out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
            }
            bool run(const string& dbName , BSONObj& cmdObj, int options, string& errmsg, BSONObjBuilder& result, bool) {
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    return passthrough( conf , cmdObj , options, result );
                }

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 10420 ,  "how could chunk manager be null!" , cm );

                BSONObj query = getQuery(cmdObj);
                set<Shard> shards;
                cm->getShardsForQuery(shards, query);

                set<BSONObj,BSONObjCmp> all;
                int size = 32;

                for ( set<Shard>::iterator i=shards.begin(), end=shards.end() ; i != end; ++i ) {
                    ShardConnection conn( *i , fullns );
                    BSONObj res;
                    bool ok = conn->runCommand( conf->getName() , cmdObj , res, options );
                    conn.done();

                    if ( ! ok ) {
                        result.appendElements( res );
                        return false;
                    }

                    BSONObjIterator it( res["values"].embeddedObject() );
                    while ( it.more() ) {
                        BSONElement nxt = it.next();
                        BSONObjBuilder temp(32);
                        temp.appendAs( nxt , "" );
                        all.insert( temp.obj() );
                    }

                }

                BSONObjBuilder b( size );
                int n=0;
                for ( set<BSONObj,BSONObjCmp>::iterator i = all.begin() ; i != all.end(); i++ ) {
                    b.appendAs( i->firstElement() , b.numStr( n++ ) );
                }

                result.appendArray( "values" , b.obj() );
                return true;
            }
        } disinctCmd;

        class FileMD5Cmd : public PublicGridCommand {
        public:
            FileMD5Cmd() : PublicGridCommand("filemd5") {}
            virtual void help( stringstream &help ) const {
                help << " example: { filemd5 : ObjectId(aaaaaaa) , root : \"fs\" }";
            }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::find);
                out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
            }
            bool run(const string& dbName , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                string fullns = dbName;
                fullns += ".";
                {
                    string root = cmdObj.getStringField( "root" );
                    if ( root.size() == 0 )
                        root = "fs";
                    fullns += root;
                }
                fullns += ".chunks";

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    return passthrough( conf , cmdObj , result );
                }

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 13091 , "how could chunk manager be null!" , cm );
                if(cm->getShardKey().key() == BSON("files_id" << 1)) {
                    BSONObj finder = BSON("files_id" << cmdObj.firstElement());

                    vector<Strategy::CommandResult> results;
                    SHARDED->commandOp(dbName, cmdObj, 0, fullns, finder, &results);
                    verify(results.size() == 1); // querying on shard key so should only talk to one shard
                    BSONObj res = results.begin()->result;

                    result.appendElements(res);
                    return res["ok"].trueValue();
                }
                else if (cm->getShardKey().key() == BSON("files_id" << 1 << "n" << 1)) {
                    int n = 0;
                    BSONObj lastResult;

                    while (true) {
                        // Theory of operation: Starting with n=0, send filemd5 command to shard
                        // with that chunk (gridfs chunk not sharding chunk). That shard will then
                        // compute a partial md5 state (passed in the "md5state" field) for all
                        // contiguous chunks that it has. When it runs out or hits a discontinuity
                        // (eg [1,2,7]) it returns what it has done so far. This is repeated as
                        // long as we keep getting more chunks. The end condition is when we go to
                        // look for chunk n and it doesn't exist. This means that the file's last
                        // chunk is n-1, so we return the computed md5 results.
                        BSONObjBuilder bb;
                        bb.appendElements(cmdObj);
                        bb.appendBool("partialOk", true);
                        bb.append("startAt", n);
                        if (!lastResult.isEmpty()){
                            bb.append(lastResult["md5state"]);
                        }
                        BSONObj shardCmd = bb.obj();

                        BSONObj finder = BSON("files_id" << cmdObj.firstElement() << "n" << n);

                        vector<Strategy::CommandResult> results;
                        try {
                            SHARDED->commandOp(dbName, shardCmd, 0, fullns, finder, &results);
                        }
                        catch( DBException& e ){
                            //This is handled below and logged
                            Strategy::CommandResult errResult;
                            errResult.shardTarget = Shard();
                            errResult.result = BSON("errmsg" << e.what() << "ok" << 0 );
                            results.push_back( errResult );
                        }

                        verify(results.size() == 1); // querying on shard key so should only talk to one shard
                        BSONObj res = results.begin()->result;
                        bool ok = res["ok"].trueValue();

                        if (!ok) {
                            // Add extra info to make debugging easier
                            result.append("failedAt", n);
                            result.append("sentCommand", shardCmd);
                            BSONForEach(e, res){
                                if (!str::equals(e.fieldName(), "errmsg"))
                                    result.append(e);
                            }

                            log() << "Sharded filemd5 failed: " << result.asTempObj() << endl;

                            errmsg = string("sharded filemd5 failed because: ") + res["errmsg"].valuestrsafe();
                            return false;
                        }

                        uassert(16246, "Shard " + conf->getName() + " is too old to support GridFS sharded by {files_id:1, n:1}",
                                res.hasField("md5state"));

                        lastResult = res;
                        int nNext = res["numChunks"].numberInt();

                        if (n == nNext){
                            // no new data means we've reached the end of the file
                            result.appendElements(res);
                            return true;
                        }
                            
                        verify(nNext > n);
                        n = nNext;
                    }

                    verify(0);
                }

                // We could support arbitrary shard keys by sending commands to all shards but I don't think we should
                errmsg = "GridFS fs.chunks collection must be sharded on either {files_id:1} or {files_id:1, n:1}";
                return false;
            }
        } fileMD5Cmd;

        class Geo2dFindNearCmd : public PublicGridCommand {
        public:
            Geo2dFindNearCmd() : PublicGridCommand( "geoNear" ) {}
            void help(stringstream& h) const { h << "http://dochub.mongodb.org/core/geo#GeospatialIndexing-geoNearCommand"; }
            virtual bool passOptions() const { return true; }
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::find);
                out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
            }
            bool run(const string& dbName , BSONObj& cmdObj, int options, string& errmsg, BSONObjBuilder& result, bool) {
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ) {
                    return passthrough( conf , cmdObj , options, result );
                }

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 13500 ,  "how could chunk manager be null!" , cm );

                BSONObj query = getQuery(cmdObj);
                set<Shard> shards;
                cm->getShardsForQuery(shards, query);

                // We support both "num" and "limit" options to control limit
                int limit = 100;
                const char* limitName = cmdObj["num"].isNumber() ? "num" : "limit";
                if (cmdObj[limitName].isNumber())
                    limit = cmdObj[limitName].numberInt();

                list< shared_ptr<Future::CommandResult> > futures;
                BSONArrayBuilder shardArray;
                for ( set<Shard>::const_iterator i=shards.begin(), end=shards.end() ; i != end ; i++ ) {
                    futures.push_back( Future::spawnCommand( i->getConnString() , dbName , cmdObj, options ) );
                    shardArray.append(i->getName());
                }

                multimap<double, BSONObj> results; // TODO: maybe use merge-sort instead
                string nearStr;
                double time = 0;
                double btreelocs = 0;
                double nscanned = 0;
                double objectsLoaded = 0;
                for ( list< shared_ptr<Future::CommandResult> >::iterator i=futures.begin(); i!=futures.end(); i++ ) {
                    shared_ptr<Future::CommandResult> res = *i;
                    if ( ! res->join() ) {
                        errmsg = res->result()["errmsg"].String();
                        return false;
                    }

                    if (res->result().hasField("near")) {
                        nearStr = res->result()["near"].String();
                    }
                    time += res->result()["stats"]["time"].Number();
                    if (!res->result()["stats"]["btreelocs"].eoo()) {
                        btreelocs += res->result()["stats"]["btreelocs"].Number();
                    }
                    nscanned += res->result()["stats"]["nscanned"].Number();
                    if (!res->result()["stats"]["objectsLoaded"].eoo()) {
                        objectsLoaded += res->result()["stats"]["objectsLoaded"].Number();
                    }

                    BSONForEach(obj, res->result()["results"].embeddedObject()) {
                        results.insert(make_pair(obj["dis"].Number(), obj.embeddedObject().getOwned()));
                    }

                    // TODO: maybe shrink results if size() > limit
                }

                result.append("ns" , fullns);
                result.append("near", nearStr);

                int outCount = 0;
                double totalDistance = 0;
                double maxDistance = 0;
                {
                    BSONArrayBuilder sub (result.subarrayStart("results"));
                    for (multimap<double, BSONObj>::const_iterator it(results.begin()), end(results.end()); it!= end && outCount < limit; ++it, ++outCount) {
                        totalDistance += it->first;
                        maxDistance = it->first; // guaranteed to be highest so far

                        sub.append(it->second);
                    }
                    sub.done();
                }

                {
                    BSONObjBuilder sub (result.subobjStart("stats"));
                    sub.append("time", time);
                    sub.append("btreelocs", btreelocs);
                    sub.append("nscanned", nscanned);
                    sub.append("objectsLoaded", objectsLoaded);
                    sub.append("avgDistance", (outCount == 0) ? 0: (totalDistance / outCount));
                    sub.append("maxDistance", maxDistance);
                    sub.append("shards", shardArray.arr());
                    sub.done();
                }

                return true;
            }
        } geo2dFindNearCmd;

        class MRCmd : public PublicGridCommand {
        public:
            AtomicUInt JOB_NUMBER;

            MRCmd() : PublicGridCommand( "mapreduce" ) {}

            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                mr::addPrivilegesRequiredForMapReduce(dbname, cmdObj, out);
            }

            string getTmpName( const string& coll ) {
                stringstream ss;
                ss << "tmp.mrs." << coll << "_" << time(0) << "_" << JOB_NUMBER++;
                return ss.str();
            }

            BSONObj fixForShards( const BSONObj& orig , const string& output , string& badShardedField , int maxChunkSizeBytes ) {
                BSONObjBuilder b;
                BSONObjIterator i( orig );
                while ( i.more() ) {
                    BSONElement e = i.next();
                    string fn = e.fieldName();
                    if (fn == "map" ||
                            fn == "mapreduce" ||
                            fn == "mapparams" ||
                            fn == "reduce" ||
                            fn == "query" ||
                            fn == "sort" ||
                            fn == "scope" ||
                            fn == "verbose" ||
                            fn == "$queryOptions") {
                        b.append( e );
                    }
                    else if ( fn == "out" ||
                              fn == "finalize" ) {
                        // we don't want to copy these
                    }
                    else {
                        badShardedField = fn;
                        return BSONObj();
                    }
                }
                b.append( "out" , output );
                b.append( "shardedFirstPass" , true );

                if ( maxChunkSizeBytes > 0 ) {
                    // will need to figure out chunks, ask shards for points
                    b.append("splitInfo", maxChunkSizeBytes);
                }

                return b.obj();
            }

            void cleanUp( const set<ServerAndQuery>& servers, string dbName, string shardResultCollection ) {
                try {
                    // drop collections with tmp results on each shard
                    for ( set<ServerAndQuery>::iterator i=servers.begin(); i!=servers.end(); i++ ) {
                        ScopedDbConnection conn(i->_server);
                        conn->dropCollection( dbName + "." + shardResultCollection );
                        conn.done();
                    }
                } catch ( std::exception e ) {
                    log() << "Cannot cleanup shard results" << causedBy( e ) << endl;
                }
            }

            bool run(const string& dbName , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                return run( dbName, cmdObj, errmsg, result, 0 );
            }

            bool run(const string& dbName , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, int retry ) {
                Timer t;

                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                // Abort after two retries, m/r is an expensive operation
                if( retry > 2 ){
                    errmsg = "shard version errors preventing parallel mapreduce, check logs for further info";
                    return false;
                }
                // Re-check shard version after 1st retry
                if( retry > 0 ){
                    versionManager.forceRemoteCheckShardVersionCB( fullns );
                }

                const string shardResultCollection = getTmpName( collection );

                BSONObj customOut;
                string finalColShort;
                string finalColLong;
                bool customOutDB = false;
                string outDB = dbName;
                BSONElement outElmt = cmdObj.getField("out");
                if (outElmt.type() == Object) {
                    // check if there is a custom output
                    BSONObj out = outElmt.embeddedObject();
                    customOut = out;
                    // mode must be 1st element
                    finalColShort = out.firstElement().str();
                    if (customOut.hasField( "db" )) {
                        customOutDB = true;
                        outDB = customOut.getField("db").str();
                    }
                    finalColLong = outDB + "." + finalColShort;
                }

                DBConfigPtr confIn = grid.getDBConfig( dbName , false );
                DBConfigPtr confOut = confIn;
                if (customOutDB) {
                    confOut = grid.getDBConfig( outDB , true );
                }

                bool shardedInput = confIn && confIn->isShardingEnabled() && confIn->isSharded( fullns );
                bool shardedOutput = customOut.getBoolField("sharded");

                if (!shardedOutput)
                    uassert( 15920 ,  "Cannot output to a non-sharded collection, a sharded collection exists" , !confOut->isSharded(finalColLong) );
                // should we also prevent going from non-sharded to sharded? during the transition client may see partial data

                long long maxChunkSizeBytes = 0;
                if (shardedOutput) {
                    // will need to figure out chunks, ask shards for points
                    maxChunkSizeBytes = cmdObj["maxChunkSizeBytes"].numberLong();
                    if ( maxChunkSizeBytes == 0 ) {
                        maxChunkSizeBytes = Chunk::MaxChunkSize;
                    }
                }

                // modify command to run on shards with output to tmp collection
                string badShardedField;
                verify( maxChunkSizeBytes < 0x7fffffff );
                BSONObj shardedCommand = fixForShards( cmdObj , shardResultCollection , badShardedField, static_cast<int>(maxChunkSizeBytes) );

                if ( ! shardedInput && ! shardedOutput && ! customOutDB ) {
                    LOG(1) << "simple MR, just passthrough" << endl;
                    return passthrough( confIn , cmdObj , result );
                }

                if ( badShardedField.size() ) {
                    errmsg = str::stream() << "unknown m/r field for sharding: " << badShardedField;
                    return false;
                }

                BSONObjBuilder timingBuilder;
                BSONObj q;
                if ( cmdObj["query"].type() == Object ) {
                    q = cmdObj["query"].embeddedObjectUserCheck();
                }

                set<Shard> shards;
                set<ServerAndQuery> servers;
                vector<Strategy::CommandResult> results;

                BSONObjBuilder shardResultsB;
                BSONObjBuilder shardCountsB;
                BSONObjBuilder aggCountsB;
                map<string,long long> countsMap;
                set< BSONObj > splitPts;
                BSONObj singleResult;
                bool ok = true;

                {
                    // take distributed lock to prevent split / migration
                    /*
                    ConnectionString config = configServer.getConnectionString();
                    DistributedLock lockSetup( config , fullns );
                    dist_lock_try dlk;


                    if (shardedInput) {
                        try{
                            int tryc = 0;
                            while ( !dlk.got() ) {
                                dlk = dist_lock_try( &lockSetup , (string)"mr-parallel" );
                                if ( ! dlk.got() ) {
                                    if ( ++tryc % 100 == 0 )
                                        warning() << "the collection metadata could not be locked for mapreduce, already locked by " << dlk.other() << endl;
                                    sleepmillis(100);
                                }
                            }
                        }
                        catch( LockException& e ){
                            errmsg = str::stream() << "error locking distributed lock for mapreduce " << causedBy( e );
                            return false;
                        }
                    }
                    */

                    try {
                        SHARDED->commandOp( dbName, shardedCommand, 0, fullns, q, &results );
                    }
                    catch( DBException& e ){
                        e.addContext( str::stream() << "could not run map command on all shards for ns " << fullns << " and query " << q );
                        throw;
                    }

                    for ( vector<Strategy::CommandResult>::iterator i = results.begin();
                            i != results.end(); ++i ) {

                    	// need to gather list of all servers even if an error happened
                        string server = i->shardTarget.getConnString();
                        servers.insert( server );
                        if ( !ok ) continue;

                        singleResult = i->result;
                        ok = singleResult["ok"].trueValue();
                        if ( !ok ) continue;

                        shardResultsB.append( server , singleResult );
                        BSONObj counts = singleResult["counts"].embeddedObjectUserCheck();
                        shardCountsB.append( server , counts );

                        // add up the counts for each shard
                        // some of them will be fixed later like output and reduce
                        BSONObjIterator j( counts );
                        while ( j.more() ) {
                            BSONElement temp = j.next();
                            countsMap[temp.fieldName()] += temp.numberLong();
                        }

                        if (singleResult.hasField("splitKeys")) {
                            BSONElement splitKeys = singleResult.getField("splitKeys");
                            vector<BSONElement> pts = splitKeys.Array();
                            for (vector<BSONElement>::iterator it = pts.begin(); it != pts.end(); ++it) {
                                splitPts.insert(it->Obj().getOwned());
                            }
                        }
                    }
                }

                if ( ! ok ) {
                    cleanUp( servers, dbName, shardResultCollection );
                    errmsg = "MR parallel processing failed: ";
                    errmsg += singleResult.toString();
                    return 0;
                }

                // build the sharded finish command
                BSONObjBuilder finalCmd;
                finalCmd.append( "mapreduce.shardedfinish" , cmdObj );
                finalCmd.append( "inputDB" , dbName );
                finalCmd.append( "shardedOutputCollection" , shardResultCollection );

                finalCmd.append( "shards" , shardResultsB.done() );
                BSONObj shardCounts = shardCountsB.done();
                finalCmd.append( "shardCounts" , shardCounts );
                timingBuilder.append( "shardProcessing" , t.millis() );

                for ( map<string,long long>::iterator i=countsMap.begin(); i!=countsMap.end(); i++ ) {
                    aggCountsB.append( i->first , i->second );
                }
                BSONObj aggCounts = aggCountsB.done();
                finalCmd.append( "counts" , aggCounts );

                Timer t2;
                long long reduceCount = 0;
                long long outputCount = 0;
                BSONObjBuilder postCountsB;

                if (!shardedOutput) {
                    LOG(1) << "MR with single shard output, NS=" << finalColLong << " primary=" << confOut->getPrimary() << endl;
                    ShardConnection conn( confOut->getPrimary() , finalColLong );
                    ok = conn->runCommand( outDB , finalCmd.obj() , singleResult );

                    BSONObj counts = singleResult.getObjectField("counts");
                    postCountsB.append(conn->getServerAddress(), counts);
                    reduceCount = counts.getIntField("reduce");
                    outputCount = counts.getIntField("output");

                    conn.done();
                } else {

                    LOG(1) << "MR with sharded output, NS=" << finalColLong << endl;

                    // create the sharded collection if needed
                    if (!confOut->isSharded(finalColLong)) {
                        // enable sharding on db
                        confOut->enableSharding();

                        // shard collection according to split points
                        BSONObj sortKey = BSON( "_id" << 1 );
                        vector<BSONObj> sortedSplitPts;
                        // points will be properly sorted using the set
                        for ( set<BSONObj>::iterator it = splitPts.begin() ; it != splitPts.end() ; ++it )
                            sortedSplitPts.push_back( *it );

                        // pre-split the collection onto all the shards for this database.
                        // Note that it's not completely safe to pre-split onto non-primary shards
                        // using the shardcollection method (a conflict may result if multiple
                        // map-reduces are writing to the same output collection, for instance).
                        // TODO: pre-split mapReduce output in a safer way.
                        set<Shard> shardSet;
                        confOut->getAllShards( shardSet );
                        vector<Shard> outShards( shardSet.begin() , shardSet.end() );

                        confOut->shardCollection( finalColLong ,
                                                  sortKey ,
                                                  true ,
                                                  &sortedSplitPts ,
                                                  &outShards );

                    }

                    map<BSONObj, int> chunkSizes;
                    {
                        // take distributed lock to prevent split / migration
                        ConnectionString config = configServer.getConnectionString();
                        DistributedLock lockSetup( config , finalColLong );
                        dist_lock_try dlk;

                        try{
                            int tryc = 0;
                            while ( !dlk.got() ) {
                                dlk = dist_lock_try( &lockSetup , (string)"mr-post-process" );
                                if ( ! dlk.got() ) {
                                    if ( ++tryc % 100 == 0 )
                                        warning() << "the collection metadata could not be locked for mapreduce, already locked by " << dlk.other() << endl;
                                    sleepmillis(100);
                                }
                            }
                        }
                        catch( LockException& e ){
                            errmsg = str::stream() << "error locking distributed lock for mapreduce " << causedBy( e );
                            return false;
                        }

                        BSONObj finalCmdObj = finalCmd.obj();
                        results.clear();

                        try {
                            SHARDED->commandOp( outDB, finalCmdObj, 0, finalColLong, BSONObj(), &results );
                            ok = true;
                        }
                        catch( DBException& e ){
                            e.addContext( str::stream() << "could not run final reduce command on all shards for ns " << fullns << ", output " << finalColLong );
                            throw;
                        }

                        for ( vector<Strategy::CommandResult>::iterator i = results.begin();
                                i != results.end(); ++i ) {

                            string server = i->shardTarget.getConnString();
                            singleResult = i->result;
                            ok = singleResult["ok"].trueValue();
                            if ( !ok ) break;

                            BSONObj counts = singleResult.getObjectField("counts");
                            reduceCount += counts.getIntField("reduce");
                            outputCount += counts.getIntField("output");
                            postCountsB.append(server, counts);

                            // get the size inserted for each chunk
                            // split cannot be called here since we already have the distributed lock
                            if (singleResult.hasField("chunkSizes")) {
                                vector<BSONElement> sizes = singleResult.getField("chunkSizes").Array();
                                for (unsigned int i = 0; i < sizes.size(); i += 2) {
                                    BSONObj key = sizes[i].Obj().getOwned();
                                    long long size = sizes[i+1].numberLong();
                                    verify( size < 0x7fffffff );
                                    chunkSizes[key] = static_cast<int>(size);
                                }
                            }
                        }
                    }

                    // do the splitting round
                    ChunkManagerPtr cm = confOut->getChunkManagerIfExists( finalColLong );
                    for ( map<BSONObj, int>::iterator it = chunkSizes.begin() ; it != chunkSizes.end() ; ++it ) {
                        BSONObj key = it->first;
                        int size = it->second;
                        verify( size < 0x7fffffff );

                        // key reported should be the chunk's minimum
                        ChunkPtr c =  cm->findIntersectingChunk(key);
                        if ( !c ) {
                            warning() << "Mongod reported " << size << " bytes inserted for key " << key << " but can't find chunk" << endl;
                        } else {
                            c->splitIfShould( size );
                        }
                    }
                }

                cleanUp( servers, dbName, shardResultCollection );

                if ( ! ok ) {
                    errmsg = "MR post processing failed: ";
                    errmsg += singleResult.toString();
                    return 0;
                }

                // copy some elements from a single result
                // annoying that we have to copy all results for inline, but no way around it
                if (singleResult.hasField("result"))
                    result.append(singleResult.getField("result"));
                else if (singleResult.hasField("results"))
                    result.append(singleResult.getField("results"));

                BSONObjBuilder countsB(32);
                // input stat is determined by aggregate MR job
                countsB.append("input", aggCounts.getField("input").numberLong());
                countsB.append("emit", aggCounts.getField("emit").numberLong());

                // reduce count is sum of all reduces that happened
                countsB.append("reduce", aggCounts.getField("reduce").numberLong() + reduceCount);

                // ouput is determined by post processing on each shard
                countsB.append("output", outputCount);
                result.append( "counts" , countsB.done() );

                timingBuilder.append( "postProcessing" , t2.millis() );

                result.append( "timeMillis" , t.millis() );
                result.append( "timing" , timingBuilder.done() );
                result.append("shardCounts", shardCounts);
                result.append("postProcessCounts", postCountsB.done());
                return 1;
            }
        } mrCmd;

        class ApplyOpsCmd : public PublicGridCommand {
        public:
            ApplyOpsCmd() : PublicGridCommand( "applyOps" ) {}
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                // applyOps can do pretty much anything, so require all privileges.
                out->push_back(Privilege(AuthorizationManager::WILDCARD_RESOURCE_NAME,
                                         getGlobalAuthorizationManager()->getAllUserActions()));
            }
            virtual bool run(const string& dbName , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                errmsg = "applyOps not allowed through mongos";
                return false;
            }
        } applyOpsCmd;


        class CompactCmd : public PublicGridCommand {
        public:
            CompactCmd() : PublicGridCommand( "compact" ) {}
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                ActionSet actions;
                actions.addAction(ActionType::compact);
                out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
            }
            virtual bool run(const string& dbName , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
                errmsg = "compact not allowed through mongos";
                return false;
            }
        } compactCmd;

        class EvalCmd : public PublicGridCommand {
        public:
            EvalCmd() : PublicGridCommand( "$eval" ) {}
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out) {
                // $eval can do pretty much anything, so require all privileges.
                out->push_back(Privilege(AuthorizationManager::WILDCARD_RESOURCE_NAME,
                                         getGlobalAuthorizationManager()->getAllUserActions()));
            }
            virtual bool run(const string& dbName,
                             BSONObj& cmdObj,
                             int,
                             string&,
                             BSONObjBuilder& result,
                             bool) {
                // $eval isn't allowed to access sharded collections, but we need to leave the
                // shard to detect that.
                DBConfigPtr conf = grid.getDBConfig( dbName , false );
                return passthrough( conf , cmdObj , result );
            }
        } evalCmd;

        /*
          Note these are in the pub_grid_cmds namespace, so they don't
          conflict with those in db/commands/pipeline_command.cpp.
         */
        class PipelineCommand :
            public PublicGridCommand {
        public:
            PipelineCommand();
            // virtuals from Command
            virtual void addRequiredPrivileges(const std::string& dbname,
                                               const BSONObj& cmdObj,
                                               std::vector<Privilege>* out);
            virtual bool run(const string &dbName , BSONObj &cmdObj,
                             int options, string &errmsg,
                             BSONObjBuilder &result, bool fromRepl);

        private:
            DocumentSourceMergeCursors::CursorIds parseCursors(
                const vector<Strategy::CommandResult>& shardResults,
                const string& fullns);

            void storePossibleCursor(const string& server, BSONObj cmdResult) const;
            void killAllCursors(const vector<Strategy::CommandResult>& shardResults);
            bool doAnyShardsNotSupportCursors(const vector<Strategy::CommandResult>& shardResults);
            bool wasMergeCursorsSupported(BSONObj cmdResult);
            void uassertCanMergeInMongos(intrusive_ptr<Pipeline> mergePipeline, BSONObj cmdObj);

            void noCursorFallback(intrusive_ptr<Pipeline> shardPipeline,
                                  intrusive_ptr<Pipeline> mergePipeline,
                                  const string& dbname,
                                  const string& fullns,
                                  int options,
                                  BSONObj cmdObj,
                                  BSONObjBuilder& result);

            // These are temporary hacks because the runCommand method doesn't report the exact
            // host the command was run on which is necessary for cursor support. The exact host
            // could be different from conn->getServerAddress() for connections that map to
            // multiple servers such as for replica sets. These also take care of registering
            // returned cursors with mongos's cursorCache.
            BSONObj aggRunCommand(DBClientBase* conn,
                                  const string& db,
                                  BSONObj cmd,
                                  int queryOptions);
            bool aggPassthrough(DBConfigPtr conf,
                                BSONObj cmd,
                                BSONObjBuilder& result,
                                int queryOptions);

        };


        /* -------------------- PipelineCommand ----------------------------- */

        static const PipelineCommand pipelineCommand;

        PipelineCommand::PipelineCommand():
            PublicGridCommand(Pipeline::commandName) {
        }

        void PipelineCommand::addRequiredPrivileges(const std::string& dbname,
                                                    const BSONObj& cmdObj,
                                                    std::vector<Privilege>* out) {
            Pipeline::addRequiredPrivileges(dbname, cmdObj, out);
        }

        bool PipelineCommand::run(const string &dbName , BSONObj &cmdObj,
                                  int options, string &errmsg,
                                  BSONObjBuilder &result, bool fromRepl) {
            const string fullns = parseNs(dbName, cmdObj);

            intrusive_ptr<ExpressionContext> pExpCtx =
                new ExpressionContext(InterruptStatusMongos::status, NamespaceString(fullns));
            pExpCtx->inRouter = true;

            /* parse the pipeline specification */
            intrusive_ptr<Pipeline> pPipeline(
                Pipeline::parseCommand(errmsg, cmdObj, pExpCtx));
            if (!pPipeline.get())
                return false; // there was some parsing error

            /*
              If the system isn't running sharded, or the target collection
              isn't sharded, pass this on to a mongod.
            */
            DBConfigPtr conf = grid.getDBConfig(dbName , true);
            massert(17015, "getDBConfig shouldn't return NULL",
                    conf);
            if (!conf->isShardingEnabled() || !conf->isSharded(fullns))
                return aggPassthrough(conf, cmdObj, result, options);

            /* split the pipeline into pieces for mongods and this mongos */
            intrusive_ptr<Pipeline> pShardPipeline(pPipeline->splitForSharded());

            // create the command for the shards
            MutableDocument commandBuilder(pShardPipeline->serialize());
            commandBuilder.setField("fromRouter", Value(true)); // this means produce output to be merged

            if (cmdObj.hasField("$queryOptions")) {
                commandBuilder.setField("$queryOptions", Value(cmdObj["$queryOptions"]));
            }

            commandBuilder.setField("cursor", Value(DOC("batchSize" << 0)));

            BSONObj shardedCommand = commandBuilder.freeze().toBson();
            BSONObjBuilder shardQueryBuilder;
            pShardPipeline->getInitialQuery(&shardQueryBuilder);
            BSONObj shardQuery(shardQueryBuilder.done());

            // Run the command on the shards
            // TODO need to make sure cursors are killed if a retry is needed
            vector<Strategy::CommandResult> shardResults;
            SHARDED->commandOp(dbName, shardedCommand, options, fullns, shardQuery, &shardResults);

            if (doAnyShardsNotSupportCursors(shardResults)) {
                killAllCursors(shardResults);
                noCursorFallback(
                        pShardPipeline, pPipeline, dbName, fullns, options, cmdObj, result);
                return true;
            }

            DocumentSourceMergeCursors::CursorIds cursorIds = parseCursors(shardResults, fullns);
            pPipeline->addInitialSource(DocumentSourceMergeCursors::create(cursorIds, pExpCtx));

            MutableDocument mergeCmd(pPipeline->serialize());

            if (cmdObj.hasField("cursor"))
                mergeCmd["cursor"] = Value(cmdObj["cursor"]);
            if (cmdObj.hasField("$queryOptions"))
                mergeCmd["$queryOptions"] = Value(cmdObj["$queryOptions"]);

            string outputNsOrEmpty;
            if (DocumentSourceOut* out = dynamic_cast<DocumentSourceOut*>(pPipeline->output())) {
                outputNsOrEmpty = out->getOutputNs().ns();
            }

            // Run merging command on primary shard of database. Need to use ShardConnection so
            // that the merging mongod is sent the config servers on connection init.
            const string mergeServer = conf->getPrimary().getConnString();
            ShardConnection conn(mergeServer, outputNsOrEmpty);
            BSONObj mergedResults = aggRunCommand(conn.get(),
                                                  dbName,
                                                  mergeCmd.freeze().toBson(),
                                                  options);
            bool ok = mergedResults["ok"].trueValue();
            conn.done();

            if (!ok && !wasMergeCursorsSupported(mergedResults)) {
                // This means that the cursors were constructed on all shards containing data
                // needed for the pipeline, but the primary shard doesn't support merging them.
                uassertCanMergeInMongos(pPipeline, cmdObj);

                pPipeline->stitch();
                pPipeline->run(result);
                return true;
            }

            // Copy output from merging (primary) shard to the output object from our command.
            // Also, propagates errmsg and code if ok == false.
            result.appendElements(mergedResults);

            return ok;
        }

        void PipelineCommand::uassertCanMergeInMongos(intrusive_ptr<Pipeline> mergePipeline,
                                                      BSONObj cmdObj) {
            uassert(17020, "All shards must support cursors to get a cursor back from aggregation",
                    !cmdObj.hasField("cursor"));

            uassert(17021, "All shards must support cursors to support new features in aggregation",
                    mergePipeline->canRunInMongos());
        }

        void PipelineCommand::noCursorFallback(intrusive_ptr<Pipeline> shardPipeline,
                                               intrusive_ptr<Pipeline> mergePipeline,
                                               const string& dbName,
                                               const string& fullns,
                                               int options,
                                               BSONObj cmdObj,
                                               BSONObjBuilder& result) {
            uassertCanMergeInMongos(mergePipeline, cmdObj);

            MutableDocument commandBuilder(shardPipeline->serialize());
            commandBuilder["fromRouter"] = Value(true);

            if (cmdObj.hasField("$queryOptions")) {
                commandBuilder["$queryOptions"] = Value(cmdObj["$queryOptions"]);
            }
            BSONObj shardedCommand = commandBuilder.freeze().toBson();

            BSONObjBuilder shardQueryBuilder;
            shardPipeline->getInitialQuery(&shardQueryBuilder);
            BSONObj shardQuery(shardQueryBuilder.done());

            // Run the command on the shards
            vector<Strategy::CommandResult> shardResults;
            SHARDED->commandOp(dbName, shardedCommand, options, fullns, shardQuery, &shardResults);

            mergePipeline->addInitialSource(
                    DocumentSourceCommandShards::create(shardResults, mergePipeline->getContext()));

            // Combine the shards' output and finish the pipeline
            mergePipeline->stitch();
            mergePipeline->run(result);
        }

        DocumentSourceMergeCursors::CursorIds PipelineCommand::parseCursors(
                const vector<Strategy::CommandResult>& shardResults,
                const string& fullns) {
            try {
                DocumentSourceMergeCursors::CursorIds cursors;
                for (size_t i = 0; i < shardResults.size(); i++) {
                    BSONObj result = shardResults[i].result;
                    uassert(17022, str::stream()
                                    << "sharded pipeline failed on shard "
                                    << shardResults[i].shardTarget.getName() << ": "
                                    << result.toString(),
                            result["ok"].trueValue());

                    BSONObj cursor = result["cursor"].Obj();

                    massert(17023, str::stream()
                                    << "shard " << shardResults[i].shardTarget.getName()
                                    << " returned non-empty first batch",
                            cursor["firstBatch"].Obj().isEmpty());
                    massert(17024, str::stream()
                                    << "shard " << shardResults[i].shardTarget.getName()
                                    << " returned cursorId 0",
                            cursor["id"].Long() != 0);
                    massert(17025, str::stream()
                                    << "shard " << shardResults[i].shardTarget.getName()
                                    << " returned different ns: " << cursor["ns"],
                            cursor["ns"].String() == fullns);

                    cursors.push_back(make_pair(shardResults[i].target, cursor["id"].Long()));
                }

                return cursors;
            }
            catch (...) {
                // Need to clean up any cursors we successfully created on the shards
                killAllCursors(shardResults);
                throw;
            }
        }

        bool PipelineCommand::doAnyShardsNotSupportCursors(
                const vector<Strategy::CommandResult>& shardResults) {
            // Note: all other errors are handled elsewhere
            for (size_t i = 0; i < shardResults.size(); i++) {
                // This is the result of requesting a cursor on a mongod <2.6. Yes, the
                // unbalanced '"' is correct.
                if (shardResults[i].result["errmsg"].str() == "unrecognized field \"cursor") {
                    return true;
                }
            }

            return false;
        }

        bool PipelineCommand::wasMergeCursorsSupported(BSONObj cmdResult) {
            // Note: all other errors are returned directly
            // This is the result of using $mergeCursors on a mongod <2.6.
            const char* errmsg = "exception: Unrecognized pipeline stage name: '$mergeCursors'";
            return cmdResult["errmsg"].str() != errmsg;
        }

        void PipelineCommand::killAllCursors(const vector<Strategy::CommandResult>& shardResults) {
            // This function must ignore and log all errors. Callers expect a best-effort attempt at
            // cleanup without exceptions. If any cursors aren't cleaned up here, they will be
            // cleaned up automatically on the shard after 10 minutes anyway.

            for (size_t i = 0; i < shardResults.size(); i++) {
                try {
                    BSONObj result = shardResults[i].result;
                    if (!result["ok"].trueValue())
                        continue;

                    long long cursor = result["cursor"]["id"].Long();
                    if (!cursor)
                        continue;

                    ScopedDbConnection conn(shardResults[i].target);
                    conn->killCursor(cursor);
                    conn.done();
                }
                catch (const DBException& e) {
                    log() << "Couldn't kill aggregation cursor on shard: "
                          << shardResults[i].target
                          << " due to DBException: " << e.toString();
                }
                catch (const std::exception& e) {
                    log() << "Couldn't kill aggregation cursor on shard: "
                          << shardResults[i].target
                          << " due to std::exception: " << e.what();
                }
                catch (...) {
                    log() << "Couldn't kill aggregation cursor on shard: "
                          << shardResults[i].target
                          << " due to non-exception";
                }
            }
        }

        BSONObj PipelineCommand::aggRunCommand(DBClientBase* conn,
                                               const string& db,
                                               BSONObj cmd,
                                               int queryOptions) {
            // Temporary hack. See comment on declaration for details.

            massert(17016, "should only be running an aggregate command here",
                    str::equals(cmd.firstElementFieldName(), "aggregate"));

            scoped_ptr<DBClientCursor> cursor(conn->query(db + ".$cmd",
                                                          cmd,
                                                          -1, // nToReturn
                                                          0, // nToSkip
                                                          NULL, // fieldsToReturn
                                                          queryOptions));
            massert(17014, "aggregate command didn't return results",
                    cursor->more());

            BSONObj result = cursor->next().getOwned();
            storePossibleCursor(cursor->originalHost(), result);
            return result;
        }

        bool PipelineCommand::aggPassthrough(DBConfigPtr conf,
                                             BSONObj cmd,
                                             BSONObjBuilder& out,
                                             int queryOptions) {
            // Temporary hack. See comment on declaration for details.

            ShardConnection conn( conf->getPrimary() , "" );
            BSONObj result = aggRunCommand(conn.get(), conf->getName(), cmd, queryOptions);
            conn.done();

            bool ok = result["ok"].trueValue();
            if (!ok && result["code"].numberInt() == SendStaleConfigCode) {
                throw RecvStaleConfigException("command failed because of stale config", result);
            }
            out.appendElements(result);
            return ok;
        }

        void PipelineCommand::storePossibleCursor(const string& server, BSONObj cmdResult) const {
            if (cmdResult["ok"].trueValue() && cmdResult.hasField("cursor")) {
                long long cursorId = cmdResult["cursor"]["id"].Long();
                if (cursorId) {
                    const string cursorNs = cmdResult["cursor"]["ns"].String();
                    cursorCache.storeRef(server, cursorId, cursorNs);
                }
            }
        }

    } // namespace pub_grid_cmds

    void Command::runAgainstRegistered(const char *ns, BSONObj& jsobj, BSONObjBuilder& anObjBuilder,
                                       int queryOptions) {
        // It should be impossible for this uassert to fail since there should be no way to get into
        // this function with any other collection name.
        uassert(16618,
                "Illegal attempt to run a command against a namespace other than $cmd.",
                nsToCollectionSubstring(ns) == "$cmd");

        BSONElement e = jsobj.firstElement();
        std::string commandName = e.fieldName();
        Command* c = e.type() ? Command::findCommand(commandName) : NULL;
        if (!c) {
            Command::appendCommandStatus(anObjBuilder,
                                         false,
                                         str::stream() << "no such cmd: " << commandName);
            return;
        }
        ClientInfo *client = ClientInfo::get();

        execCommandClientBasic(c, *client, queryOptions, ns, jsobj, anObjBuilder, false);
    }

} // namespace mongo

