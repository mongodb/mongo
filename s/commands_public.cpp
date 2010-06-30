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
*/

#include "pch.h"
#include "../util/message.h"
#include "../db/dbmessage.h"
#include "../client/connpool.h"
#include "../client/parallel.h"
#include "../db/commands.h"

#include "config.h"
#include "chunk.h"
#include "strategy.h"

namespace mongo {

    namespace dbgrid_pub_cmds {
        
        class PublicGridCommand : public Command {
        public:
            PublicGridCommand( const char * n ) : Command( n ){
            }
            virtual bool slaveOk() const {
                return true;
            }
            virtual bool adminOnly() const {
                return false;
            }

            // all grid commands are designed not to lock
            virtual LockType locktype() const { return NONE; } 

        protected:
            bool passthrough( DBConfigPtr conf, const BSONObj& cmdObj , BSONObjBuilder& result ){
                return _passthrough(conf->getName(), conf, cmdObj, result);
            }
            bool adminPassthrough( DBConfigPtr conf, const BSONObj& cmdObj , BSONObjBuilder& result ){
                return _passthrough("admin", conf, cmdObj, result);
            }
            
        private:
            bool _passthrough(const string& db,  DBConfigPtr conf, const BSONObj& cmdObj , BSONObjBuilder& result ){
                ShardConnection conn( conf->getPrimary() , "" );
                BSONObj res;
                bool ok = conn->runCommand( db , cmdObj , res );
                result.appendElements( res );
                conn.done();
                return ok;
            }
        };
        
        class NotAllowedOnShardedCollectionCmd : public PublicGridCommand {
        public:
            NotAllowedOnShardedCollectionCmd( const char * n ) : PublicGridCommand( n ){}

            virtual string getFullNS( const string& dbName , const BSONObj& cmdObj ) = 0;
            
            virtual bool run(const string& dbName , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string fullns = getFullNS( dbName , cmdObj );
                
                DBConfigPtr conf = grid.getDBConfig( dbName , false );
                
                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ){
                    return passthrough( conf , cmdObj , result );
                }
                errmsg = "can't do command: " + name + " on sharded collection";
                return false;
            }
        };
        
        // ----

        class DropCmd : public PublicGridCommand {
        public:
            DropCmd() : PublicGridCommand( "drop" ){}
            bool run(const string& dbName , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;
                
                DBConfigPtr conf = grid.getDBConfig( dbName , false );
                
                log() << "DROP: " << fullns << endl;
                
                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ){
                    return passthrough( conf , cmdObj , result );
                }
                
                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 10418 ,  "how could chunk manager be null!" , cm );
                
                cm->drop( cm );

                return 1;
            }
        } dropCmd;

        class DropDBCmd : public PublicGridCommand {
        public:
            DropDBCmd() : PublicGridCommand( "dropDatabase" ){}
            bool run(const string& dbName , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                
                BSONElement e = cmdObj.firstElement();
                
                if ( ! e.isNumber() || e.number() != 1 ){
                    errmsg = "invalid params";
                    return 0;
                }
                
                DBConfigPtr conf = grid.getDBConfig( dbName , false );
                
                log() << "DROP DATABASE: " << dbName << endl;

                if ( ! conf ){
                    log(1) << "  passing though drop database for: " << dbName << endl;
                    return passthrough( conf , cmdObj , result );
                }
                
                if ( ! conf->dropDatabase( errmsg ) )
                    return false;

                result.append( "dropped" , dbName );
                return true;
            }
        } dropDBCmd;

        class RenameCollectionCmd : public PublicGridCommand {
        public:
            RenameCollectionCmd() : PublicGridCommand( "renameCollection" ){}
            bool run(const string& dbName, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string fullnsFrom = cmdObj.firstElement().valuestrsafe();
                string dbNameFrom = nsToDatabase( fullnsFrom.c_str() );
                DBConfigPtr confFrom = grid.getDBConfig( dbNameFrom , false );

                string fullnsTo = cmdObj["to"].valuestrsafe();
                string dbNameTo = nsToDatabase( fullnsTo.c_str() );
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

        class CountCmd : public PublicGridCommand {
        public:
            CountCmd() : PublicGridCommand("count") { }
            bool run(const string& dbName, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;
                
                BSONObj filter;
                if ( cmdObj["query"].isABSONObj() )
                    filter = cmdObj["query"].Obj();
                
                DBConfigPtr conf = grid.getDBConfig( dbName , false );
                
                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ){
                    ShardConnection conn( conf->getPrimary() , fullns );
                    result.append( "n" , (double)conn->count( fullns , filter ) );
                    conn.done();
                    return true;
                }
                
                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 10419 ,  "how could chunk manager be null!" , cm );
                
                vector<shared_ptr<ChunkRange> > chunks;
                cm->getChunksForQuery( chunks , filter );
                
                unsigned long long total = 0;
                for ( vector<shared_ptr<ChunkRange> >::iterator i = chunks.begin() ; i != chunks.end() ; i++ ){
                    shared_ptr<ChunkRange> c = *i;
                    total += c->countObjects( filter );
                }
                
                result.append( "n" , (double)total );
                return true;
            }
        } countCmd;

        class CollectionStats : public PublicGridCommand {
        public:
            CollectionStats() : PublicGridCommand("collstats") { }
            bool run(const string& dbName , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;
                
                DBConfigPtr conf = grid.getDBConfig( dbName , false );
                
                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ){
                    result.appendBool("sharded", false);
                    return passthrough( conf , cmdObj , result);
                }
                result.appendBool("sharded", true);

                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 12594 ,  "how could chunk manager be null!" , cm );

                set<Shard> servers;
                cm->getAllShards(servers);
                
                BSONObjBuilder shardStats;
                long long count=0;
                long long size=0;
                long long storageSize=0;
                int nindexes=0;
                for ( set<Shard>::iterator i=servers.begin(); i!=servers.end(); i++ ){
                    ShardConnection conn( *i , fullns );
                    BSONObj res;
                    if ( ! conn->runCommand( dbName , cmdObj , res ) ){
                        errmsg = "failed on shard: " + res.toString();
                        return false;
                    }
                    conn.done();

                    count += res["count"].numberLong();
                    size += res["size"].numberLong();
                    storageSize += res["storageSize"].numberLong();

                    if (nindexes)
                        massert(12595, "nindexes should be the same on all shards!", nindexes == res["nindexes"].numberInt());
                    else
                        nindexes = res["nindexes"].numberInt();

                    shardStats.append(i->getName(), res);
                }

                result.append("ns", fullns);
                result.appendNumber("count", count);
                result.appendNumber("size", size);
                result.appendNumber("storageSize", storageSize);
                result.append("nindexes", nindexes);

                result.append("nchunks", cm->numChunks());
                result.append("shards", shardStats.obj());
                
                return true;
            }
        } collectionStatsCmd;

        class FindAndModifyCmd : public PublicGridCommand {
        public:
            FindAndModifyCmd() : PublicGridCommand("findandmodify") { }
            bool run(const string& dbName, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;
                
                BSONObj filter = cmdObj.getObjectField("query");
                
                DBConfigPtr conf = grid.getDBConfig( dbName , false );
                
                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ){
                    return passthrough( conf , cmdObj , result);
                }
                
                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 13002 ,  "how could chunk manager be null!" , cm );
                
                vector<shared_ptr<ChunkRange> > chunks;
                cm->getChunksForQuery( chunks , filter );

                BSONObj sort = cmdObj.getObjectField("sort");
                if (!sort.isEmpty()){
                    ShardKeyPattern& sk = cm->getShardKey();
                    {
                        BSONObjIterator k (sk.key());
                        BSONObjIterator s (sort);
                        bool good = true;
                        while (k.more()){
                            if (!s.more()){
                                good = false;
                                break;
                            }

                            BSONElement ke = k.next();
                            BSONElement se = s.next();

                            // TODO consider values when we support compound keys
                            if (strcmp(ke.fieldName(), se.fieldName()) != 0){
                                good = false;
                                break;
                            }
                        }

                        uassert(13001, "Sort must match shard key for sharded findandmodify", good);
                    }

                    std::sort(chunks.begin(), chunks.end(), ChunkCmp(sort));
                }

                for ( vector<shared_ptr<ChunkRange> >::iterator i = chunks.begin() ; i != chunks.end() ; i++ ){
                    shared_ptr<ChunkRange> c = *i;

                    BSONObj myCommand = fixCmdObj( cmdObj , c );
                    ShardConnection conn( c->getShard() , fullns );
                    BSONObj res;
                    bool ok = conn->runCommand( conf->getName() , myCommand , res );
                    conn.done();

                    if (ok || (strcmp(res["errmsg"].valuestrsafe(), "No matching object found") != 0)){
                        result.appendElements(res);
                        return ok;
                    }
                }
                
                return true;
            }

        private:
            BSONObj fixCmdObj(const BSONObj& cmdObj, const shared_ptr<ChunkRange> chunk){
                assert(chunk);

                BSONObjBuilder b;
                BSONObjIterator i(cmdObj);
                bool foundQuery = false;
                while (i.more()){
                    BSONElement e = i.next();
                    if (strcmp(e.fieldName(), "query") != 0){
                        b.append(e);
                    }else{
                        foundQuery = true;
                        b.append("query", ClusteredCursor::concatQuery(e.embeddedObjectUserCheck(), chunk->getFilter()));
                    }
                }

                if (!foundQuery)
                    b.append("query", chunk->getFilter());

                return b.obj();
            }

        } findAndModifyCmd;

        class ConvertToCappedCmd : public NotAllowedOnShardedCollectionCmd  {
        public:
            ConvertToCappedCmd() : NotAllowedOnShardedCollectionCmd("convertToCapped"){}
            
            virtual string getFullNS( const string& dbName , const BSONObj& cmdObj ){
                return dbName + "." + cmdObj.firstElement().valuestrsafe();
            }
            
        } convertToCappedCmd;


        class GroupCmd : public NotAllowedOnShardedCollectionCmd  {
        public:
            GroupCmd() : NotAllowedOnShardedCollectionCmd("group"){}
            
            virtual string getFullNS( const string& dbName , const BSONObj& cmdObj ){
                return dbName + "." + cmdObj.firstElement().embeddedObjectUserCheck()["ns"].valuestrsafe();
            }
            
        } groupCmd;

        class DistinctCmd : public PublicGridCommand {
        public:
            DistinctCmd() : PublicGridCommand("distinct"){}
            virtual void help( stringstream &help ) const {
                help << "{ distinct : 'collection name' , key : 'a.b' }";
            }
            bool run(const string& dbName , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                DBConfigPtr conf = grid.getDBConfig( dbName , false );
                
                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ){
                    return passthrough( conf , cmdObj , result );
                }
                
                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 10420 ,  "how could chunk manager be null!" , cm );
                
                vector<shared_ptr<ChunkRange> > chunks;
                cm->getChunksForQuery( chunks , BSONObj() );
                
                set<BSONObj,BSONObjCmp> all;
                int size = 32;
                
                for ( vector<shared_ptr<ChunkRange> >::iterator i = chunks.begin() ; i != chunks.end() ; i++ ){
                    shared_ptr<ChunkRange> c = *i;

                    ShardConnection conn( c->getShard() , fullns );
                    BSONObj res;
                    bool ok = conn->runCommand( conf->getName() , cmdObj , res );
                    conn.done();
                    
                    if ( ! ok ){
                        result.appendElements( res );
                        return false;
                    }
                    
                    BSONObjIterator it( res["values"].embeddedObjectUserCheck() );
                    while ( it.more() ){
                        BSONElement nxt = it.next();
                        BSONObjBuilder temp(32);
                        temp.appendAs( nxt , "x" );
                        all.insert( temp.obj() );
                    }

                }
                
                BSONObjBuilder b( size );
                int n=0;
                for ( set<BSONObj,BSONObjCmp>::iterator i = all.begin() ; i != all.end(); i++ ){
                    b.appendAs( i->firstElement() , b.numStr( n++ ).c_str() );
                }
                
                result.appendArray( "values" , b.obj() );
                return true;
            }
        } disinctCmd;

        class FileMD5Cmd : public PublicGridCommand {
        public:
            FileMD5Cmd() : PublicGridCommand("filemd5"){}
            virtual void help( stringstream &help ) const {
                help << " example: { filemd5 : ObjectId(aaaaaaa) , root : \"fs\" }";
            }
            bool run(const string& dbName , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
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
                
                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ){
                    return passthrough( conf , cmdObj , result );
                }
                
                ChunkManagerPtr cm = conf->getChunkManager( fullns );
                massert( 13091 , "how could chunk manager be null!" , cm );
                uassert( 13092 , "GridFS chunks collection can only be sharded on files_id", cm->getShardKey().key() == BSON("files_id" << 1));

                ChunkPtr chunk = cm->findChunk( BSON("files_id" << cmdObj.firstElement()) );
                
                ShardConnection conn( chunk->getShard() , fullns );
                BSONObj res;
                bool ok = conn->runCommand( conf->getName() , cmdObj , res );
                conn.done();

                result.appendElements(res);
                return ok;
            }
        } fileMD5Cmd;

        class MRCmd : public PublicGridCommand {
        public:
            MRCmd() : PublicGridCommand( "mapreduce" ){}
            
            string getTmpName( const string& coll ){
                static int inc = 1;
                stringstream ss;
                ss << "tmp.mrs." << coll << "_" << time(0) << "_" << inc++;
                return ss.str();
            }

            BSONObj fixForShards( const BSONObj& orig , const string& output ){
                BSONObjBuilder b;
                BSONObjIterator i( orig );
                while ( i.more() ){
                    BSONElement e = i.next();
                    string fn = e.fieldName();
                    if ( fn == "map" || 
                         fn == "mapreduce" || 
                         fn == "reduce" ||
                         fn == "query" ||
                         fn == "sort" ||
                         fn == "verbose" ){
                        b.append( e );
                    }
                    else if ( fn == "keeptemp" ||
                              fn == "out" ||
                              fn == "finalize" ){
                        // we don't want to copy these
                    }
                    else {
                        uassert( 10177 ,  (string)"don't know mr field: " + fn , 0 );
                    }
                }
                b.append( "out" , output );
                return b.obj();
            }
            
            bool run(const string& dbName , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                Timer t;

                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;

                DBConfigPtr conf = grid.getDBConfig( dbName , false );

                if ( ! conf || ! conf->isShardingEnabled() || ! conf->isSharded( fullns ) ){
                    return passthrough( conf , cmdObj , result );
                }
                
                BSONObjBuilder timingBuilder;

                ChunkManagerPtr cm = conf->getChunkManager( fullns );

                BSONObj q;
                if ( cmdObj["query"].type() == Object ){
                    q = cmdObj["query"].embeddedObjectUserCheck();
                }
                
                vector<shared_ptr<ChunkRange> > chunks;
                cm->getChunksForQuery( chunks , q );
                
                const string shardedOutputCollection = getTmpName( collection );
                
                BSONObj shardedCommand = fixForShards( cmdObj , shardedOutputCollection );
                
                BSONObjBuilder finalCmd;
                finalCmd.append( "mapreduce.shardedfinish" , cmdObj );
                finalCmd.append( "shardedOutputCollection" , shardedOutputCollection );
                
                list< shared_ptr<Future::CommandResult> > futures;
                
                for ( vector<shared_ptr<ChunkRange> >::iterator i = chunks.begin() ; i != chunks.end() ; i++ ){
                    shared_ptr<ChunkRange> c = *i;
                    futures.push_back( Future::spawnCommand( c->getShard().getConnString() , dbName , shardedCommand ) );
                }
                
                BSONObjBuilder shardresults;
                for ( list< shared_ptr<Future::CommandResult> >::iterator i=futures.begin(); i!=futures.end(); i++ ){
                    shared_ptr<Future::CommandResult> res = *i;
                    if ( ! res->join() ){
                        errmsg = "mongod mr failed: ";
                        errmsg += res->result().toString();
                        return 0;
                    }
                    shardresults.append( res->getServer() , res->result() );
                }
                
                finalCmd.append( "shards" , shardresults.obj() );
                timingBuilder.append( "shards" , t.millis() );

                Timer t2;
                ShardConnection conn( conf->getPrimary() , fullns );
                BSONObj finalResult;
                bool ok = conn->runCommand( dbName , finalCmd.obj() , finalResult );
                conn.done();

                if ( ! ok ){
                    errmsg = "final reduce failed: ";
                    errmsg += finalResult.toString();
                    return 0;
                }
                timingBuilder.append( "final" , t2.millis() );

                result.appendElements( finalResult );
                result.append( "timeMillis" , t.millis() );
                result.append( "timing" , timingBuilder.obj() );
                
                return 1;
            }
        } mrCmd;
        
        class ApplyOpsCmd : public PublicGridCommand {
        public:
            ApplyOpsCmd() : PublicGridCommand( "applyOps" ){}
            
            virtual bool run(const string& dbName , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                errmsg = "applyOps not allowed through mongos";
                return false;
            }
            
        } applyOpsCmd;
        
    }

}
