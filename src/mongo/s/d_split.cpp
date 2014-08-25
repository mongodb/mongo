// @file  d_split.cpp

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
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <map>
#include <string>
#include <vector>

#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_legacy.h"
#include "mongo/db/instance.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/s/chunk.h" // for static genID only
#include "mongo/s/chunk_version.h"
#include "mongo/s/config.h"
#include "mongo/s/d_logic.h"
#include "mongo/s/distlock.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/type_chunk.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {

    class CmdMedianKey : public Command {
    public:
        CmdMedianKey() : Command( "medianKey" ) {}
        virtual bool slaveOk() const { return true; }
        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual void help( stringstream &help ) const {
            help << "Deprecated internal command. Use splitVector command instead. \n";
        }
        // No auth required as this command no longer does anything.
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {}
        bool run(OperationContext* txn, const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            errmsg = "medianKey command no longer supported. Calling this indicates mismatch between mongo versions.";
            return false;
        }
    } cmdMedianKey;

    class CheckShardingIndex : public Command {
    public:
        CheckShardingIndex() : Command( "checkShardingIndex" , false ) {}
        virtual bool slaveOk() const { return false; }
        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual void help( stringstream &help ) const {
            help << "Internal command.\n";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::find);
            out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
        }

        std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
            return parseNsFullyQualified(dbname, cmdObj);
        }

        bool run(OperationContext* txn, const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {

            std::string ns = parseNs(dbname, jsobj);
            BSONObj keyPattern = jsobj.getObjectField( "keyPattern" );

            if ( keyPattern.isEmpty() ) {
                errmsg = "no key pattern found in checkShardingindex";
                return false;
            }

            if ( keyPattern.nFields() == 1 && str::equals( "_id" , keyPattern.firstElementFieldName() ) ) {
                result.appendBool( "idskip" , true );
                return true;
            }

            BSONObj min = jsobj.getObjectField( "min" );
            BSONObj max = jsobj.getObjectField( "max" );
            if ( min.isEmpty() != max.isEmpty() ) {
                errmsg = "either provide both min and max or leave both empty";
                return false;
            }

            Client::ReadContext ctx(txn, ns);
            Collection* collection = ctx.ctx().db()->getCollection( txn, ns );
            if ( !collection ) {
                errmsg = "ns not found";
                return false;
            }

            IndexDescriptor *idx =
                collection->getIndexCatalog()->findIndexByPrefix( txn,
                                                                  keyPattern,
                                                                  true );  /* require single key */
            if ( idx == NULL ) {
                errmsg = "couldn't find valid index for shard key";
                return false;
            }
            // extend min to get (min, MinKey, MinKey, ....)
            KeyPattern kp( idx->keyPattern() );
            min = Helpers::toKeyFormat( kp.extendRangeBound( min, false ) );
            if  ( max.isEmpty() ) {
                // if max not specified, make it (MaxKey, Maxkey, MaxKey...)
                max = Helpers::toKeyFormat( kp.extendRangeBound( max, true ) );
            } else {
                // otherwise make it (max,MinKey,MinKey...) so that bound is non-inclusive
                max = Helpers::toKeyFormat( kp.extendRangeBound( max, false ) );
            }

            auto_ptr<PlanExecutor> exec(InternalPlanner::indexScan(txn, collection, idx,
                                                                   min, max, false,
                                                                   InternalPlanner::FORWARD));

            // Find the 'missingField' value used to represent a missing document field in a key of
            // this index.
            // NOTE A local copy of 'missingField' is made because indices may be
            // invalidated during a db lock yield.
            BSONObj missingFieldObj = IndexLegacy::getMissingField(txn, collection, idx->infoObj());
            BSONElement missingField = missingFieldObj.firstElement();
            
            // for now, the only check is that all shard keys are filled
            // a 'missingField' valued index key is ok if the field is present in the document,
            // TODO if $exist for nulls were picking the index, it could be used instead efficiently
            int keyPatternLength = keyPattern.nFields();

            DiskLoc loc;
            BSONObj currKey;
            while (PlanExecutor::ADVANCED == exec->getNext(&currKey, &loc)) {
                //check that current key contains non missing elements for all fields in keyPattern
                BSONObjIterator i( currKey );
                for( int k = 0; k < keyPatternLength ; k++ ) {
                    if( ! i.more() ) {
                        errmsg = str::stream() << "index key " << currKey
                                               << " too short for pattern " << keyPattern;
                        return false;
                    }
                    BSONElement currKeyElt = i.next();
                    
                    if ( !currKeyElt.eoo() && !currKeyElt.valuesEqual( missingField ) )
                        continue;

                    // This is a fetch, but it's OK.  The underlying code won't throw a page fault
                    // exception.
                    BSONObj obj = collection->docFor(txn, loc);
                    BSONObjIterator j( keyPattern );
                    BSONElement real;
                    for ( int x=0; x <= k; x++ )
                        real = j.next();

                    real = obj.getFieldDotted( real.fieldName() );

                    if ( real.type() )
                        continue;
                    
                    ostringstream os;
                    os << "found missing value in key " << currKey << " for doc: "
                       << ( obj.hasField( "_id" ) ? obj.toString() : obj["_id"].toString() );
                    log() << "checkShardingIndex for '" << ns << "' failed: " << os.str() << endl;
                    
                    errmsg = os.str();
                    return false;
                }
            }

            return true;
        }
    } cmdCheckShardingIndex;

    BSONObj prettyKey(const BSONObj& keyPattern, const BSONObj& key) {
        return key.replaceFieldNames(keyPattern).clientReadable();
    }

    class SplitVector : public Command {
    public:
        SplitVector() : Command( "splitVector" , false ) {}
        virtual bool slaveOk() const { return false; }
        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual void help( stringstream &help ) const {
            help <<
                 "Internal command.\n"
                 "examples:\n"
                 "  { splitVector : \"blog.post\" , keyPattern:{x:1} , min:{x:10} , max:{x:20}, maxChunkSize:200 }\n"
                 "  maxChunkSize unit in MBs\n"
                 "  May optionally specify 'maxSplitPoints' and 'maxChunkObjects' to avoid traversing the whole chunk\n"
                 "  \n"
                 "  { splitVector : \"blog.post\" , keyPattern:{x:1} , min:{x:10} , max:{x:20}, force: true }\n"
                 "  'force' will produce one split point even if data is small; defaults to false\n"
                 "NOTE: This command may take a while to run";
        }
        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            if (!client->getAuthorizationSession()->isAuthorizedForActionsOnResource(
                    ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                    ActionType::splitVector)) {
                return Status(ErrorCodes::Unauthorized, "Unauthorized");
            }
            return Status::OK();
        }
        virtual std::string parseNs(const string& dbname, const BSONObj& cmdObj) const {
            return parseNsFullyQualified(dbname, cmdObj);
        }
        bool run(OperationContext* txn, const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {

            //
            // 1.a We'll parse the parameters in two steps. First, make sure the we can use the split index to get
            //     a good approximation of the size of the chunk -- without needing to access the actual data.
            //

            const std::string ns = parseNs(dbname, jsobj);
            BSONObj keyPattern = jsobj.getObjectField( "keyPattern" );

            if ( keyPattern.isEmpty() ) {
                errmsg = "no key pattern found in splitVector";
                return false;
            }

            // If min and max are not provided use the "minKey" and "maxKey" for the sharding key pattern.
            BSONObj min = jsobj.getObjectField( "min" );
            BSONObj max = jsobj.getObjectField( "max" );
            if ( min.isEmpty() != max.isEmpty() ){
                errmsg = "either provide both min and max or leave both empty";
                return false;
            }

            long long maxSplitPoints = 0;
            BSONElement maxSplitPointsElem = jsobj[ "maxSplitPoints" ];
            if ( maxSplitPointsElem.isNumber() ) {
                maxSplitPoints = maxSplitPointsElem.numberLong();
            }

            long long maxChunkObjects = Chunk::MaxObjectPerChunk;
            BSONElement MaxChunkObjectsElem = jsobj[ "maxChunkObjects" ];
            if ( MaxChunkObjectsElem.isNumber() ) {
                maxChunkObjects = MaxChunkObjectsElem.numberLong();
            }

            vector<BSONObj> splitKeys;

            {
                // Get the size estimate for this namespace
                Client::ReadContext ctx(txn, ns);
                Collection* collection = ctx.ctx().db()->getCollection( txn, ns );
                if ( !collection ) {
                    errmsg = "ns not found";
                    return false;
                }

                // Allow multiKey based on the invariant that shard keys must be single-valued.
                // Therefore, any multi-key index prefixed by shard key cannot be multikey over
                // the shard key fields.
                IndexDescriptor *idx =
                    collection->getIndexCatalog()->findIndexByPrefix( txn,
                                                                      keyPattern,
                                                                      false );
                if ( idx == NULL ) {
                    errmsg = (string)"couldn't find index over splitting key " +
                             keyPattern.clientReadable().toString();
                    return false;
                }
                // extend min to get (min, MinKey, MinKey, ....)
                KeyPattern kp( idx->keyPattern() );
                min = Helpers::toKeyFormat( kp.extendRangeBound ( min, false ) );
                if  ( max.isEmpty() ) {
                    // if max not specified, make it (MaxKey, Maxkey, MaxKey...)
                    max = Helpers::toKeyFormat( kp.extendRangeBound( max, true ) );
                } else {
                    // otherwise make it (max,MinKey,MinKey...) so that bound is non-inclusive
                    max = Helpers::toKeyFormat( kp.extendRangeBound( max, false ) );
                }

                const long long recCount = collection->numRecords(txn);
                const long long dataSize = collection->dataSize(txn);

                //
                // 1.b Now that we have the size estimate, go over the remaining parameters and apply any maximum size
                //     restrictions specified there.
                //
                
                // 'force'-ing a split is equivalent to having maxChunkSize be the size of the current chunk, i.e., the
                // logic below will split that chunk in half
                long long maxChunkSize = 0;
                bool forceMedianSplit = false;
                {
                    BSONElement maxSizeElem = jsobj[ "maxChunkSize" ];
                    BSONElement forceElem = jsobj[ "force" ];
                    
                    if ( forceElem.trueValue() ) {
                        forceMedianSplit = true;
                        // This chunk size is effectively ignored if force is true
                        maxChunkSize = dataSize;
                        
                    }
                    else if ( maxSizeElem.isNumber() ) {
                        maxChunkSize = maxSizeElem.numberLong() * 1<<20;
                        
                    }
                    else {
                        maxSizeElem = jsobj["maxChunkSizeBytes"];
                        if ( maxSizeElem.isNumber() ) {
                            maxChunkSize = maxSizeElem.numberLong();
                        }
                    }

                    // We need a maximum size for the chunk, unless we're not actually capable of finding any
                    // split points.
                    if ( maxChunkSize <= 0 && recCount != 0 ) {
                        errmsg = "need to specify the desired max chunk size (maxChunkSize or maxChunkSizeBytes)";
                        return false;
                    }
                }
                
                
                // If there's not enough data for more than one chunk, no point continuing.
                if ( dataSize < maxChunkSize || recCount == 0 ) {
                    vector<BSONObj> emptyVector;
                    result.append( "splitKeys" , emptyVector );
                    return true;
                }
                
                log() << "request split points lookup for chunk " << ns << " " << min << " -->> " << max << endl;
                
                // We'll use the average object size and number of object to find approximately how many keys
                // each chunk should have. We'll split at half the maxChunkSize or maxChunkObjects, if
                // provided.
                const long long avgRecSize = dataSize / recCount;
                long long keyCount = maxChunkSize / (2 * avgRecSize);
                if ( maxChunkObjects && ( maxChunkObjects < keyCount ) ) {
                    log() << "limiting split vector to " << maxChunkObjects << " (from " << keyCount << ") objects " << endl;
                    keyCount = maxChunkObjects;
                }
                
                //
                // 2. Traverse the index and add the keyCount-th key to the result vector. If that key
                //    appeared in the vector before, we omit it. The invariant here is that all the
                //    instances of a given key value live in the same chunk.
                //
                
                Timer timer;
                long long currCount = 0;
                long long numChunks = 0;
                
                auto_ptr<PlanExecutor> exec(
                    InternalPlanner::indexScan(txn, collection, idx, min, max,
                    false, InternalPlanner::FORWARD));

                BSONObj currKey;
                PlanExecutor::ExecState state = exec->getNext(&currKey, NULL);
                if (PlanExecutor::ADVANCED != state) {
                    errmsg = "can't open a cursor for splitting (desired range is possibly empty)";
                    return false;
                }
                
                // Use every 'keyCount'-th key as a split point. We add the initial key as a sentinel, to be removed
                // at the end. If a key appears more times than entries allowed on a chunk, we issue a warning and
                // split on the following key.
                set<BSONObj> tooFrequentKeys;
                splitKeys.push_back(prettyKey(idx->keyPattern(), currKey.getOwned()).extractFields( keyPattern ) );

                while ( 1 ) {
                    while (PlanExecutor::ADVANCED == state) {
                        currCount++;
                        
                        if ( currCount > keyCount && !forceMedianSplit ) {
                            currKey = prettyKey(idx->keyPattern(), currKey.getOwned()).extractFields(keyPattern);
                            // Do not use this split key if it is the same used in the previous split point.
                            if ( currKey.woCompare( splitKeys.back() ) == 0 ) {
                                tooFrequentKeys.insert( currKey.getOwned() );
                            }
                            else {
                                splitKeys.push_back( currKey.getOwned() );
                                currCount = 0;
                                numChunks++;
                                LOG(4) << "picked a split key: " << currKey << endl;
                            }
                        }

                        // Stop if we have enough split points.
                        if ( maxSplitPoints && ( numChunks >= maxSplitPoints ) ) {
                            log() << "max number of requested split points reached (" << numChunks
                                  << ") before the end of chunk " << ns << " " << min << " -->> " << max
                                  << endl;
                            break;
                        }

                        state = exec->getNext(&currKey, NULL);
                    }
                    
                    if ( ! forceMedianSplit )
                        break;
                    
                    //
                    // If we're forcing a split at the halfway point, then the first pass was just
                    // to count the keys, and we still need a second pass.
                    //

                    forceMedianSplit = false;
                    keyCount = currCount / 2;
                    currCount = 0;
                    log() << "splitVector doing another cycle because of force, keyCount now: " << keyCount << endl;

                    exec.reset(InternalPlanner::indexScan(txn, collection, idx, min, max,
                                                            false, InternalPlanner::FORWARD));

                    state = exec->getNext(&currKey, NULL);
                }

                //
                // 3. Format the result and issue any warnings about the data we gathered while traversing the
                //    index
                //
                
                // Warn for keys that are more numerous than maxChunkSize allows.
                for ( set<BSONObj>::const_iterator it = tooFrequentKeys.begin(); it != tooFrequentKeys.end(); ++it ) {
                    warning() << "chunk is larger than " << maxChunkSize
                              << " bytes because of key " << prettyKey(idx->keyPattern(), *it ) << endl;
                }
                
                // Remove the sentinel at the beginning before returning
                splitKeys.erase( splitKeys.begin() );
                
                if (timer.millis() > serverGlobalParams.slowMS) {
                    warning() << "Finding the split vector for " <<  ns << " over "<< keyPattern
                              << " keyCount: " << keyCount << " numSplits: " << splitKeys.size() 
                              << " lookedAt: " << currCount << " took " << timer.millis() << "ms"
                              << endl;
                }
                
                // Warning: we are sending back an array of keys but are currently limited to
                // 4MB work of 'result' size. This should be okay for now.

                result.append( "timeMillis", timer.millis() );
            }

            result.append( "splitKeys" , splitKeys );
            return true;

        }
    } cmdSplitVector;

    class SplitChunkCommand : public Command {
    public:
        SplitChunkCommand() : Command( "splitChunk" ) {}
        virtual void help( stringstream& help ) const {
            help <<
                 "internal command usage only\n"
                 "example:\n"
                 " { splitChunk:\"db.foo\" , keyPattern: {a:1} , min : {a:100} , max: {a:200} { splitKeys : [ {a:150} , ... ]}";
        }

        virtual bool slaveOk() const { return false; }
        virtual bool adminOnly() const { return true; }
        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            if (!client->getAuthorizationSession()->isAuthorizedForActionsOnResource(
                    ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                    ActionType::splitChunk)) {
                return Status(ErrorCodes::Unauthorized, "Unauthorized");
            }
            return Status::OK();
        }
        virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
            return parseNsFullyQualified(dbname, cmdObj);
        }
        bool run(OperationContext* txn, const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {

            //
            // 1. check whether parameters passed to splitChunk are sound
            //

            const string ns = parseNs(dbname, cmdObj);
            if ( ns.empty() ) {
                errmsg  = "need to specify namespace in command";
                return false;
            }

            const BSONObj keyPattern = cmdObj["keyPattern"].Obj();
            if ( keyPattern.isEmpty() ) {
                errmsg = "need to specify the key pattern the collection is sharded over";
                return false;
            }

            const BSONObj min = cmdObj["min"].Obj();
            if ( min.isEmpty() ) {
                errmsg = "need to specify the min key for the chunk";
                return false;
            }

            const BSONObj max = cmdObj["max"].Obj();
            if ( max.isEmpty() ) {
                errmsg = "need to specify the max key for the chunk";
                return false;
            }

            const string shardName = cmdObj["from"].str();
            if ( shardName.empty() ) {
                errmsg = "need specify server to split chunk at";
                return false;
            }

            const BSONObj splitKeysElem = cmdObj["splitKeys"].Obj();
            if ( splitKeysElem.isEmpty() ) {
                errmsg = "need to provide the split points to chunk over";
                return false;
            }
            vector<BSONObj> splitKeys;
            BSONObjIterator it( splitKeysElem );
            while ( it.more() ) {
                splitKeys.push_back( it.next().Obj().getOwned() );
            }

            const BSONElement shardId = cmdObj["shardId"];
            if ( shardId.eoo() ) {
                errmsg = "need to provide shardId";
                return false;
            }

            //
            // Get sharding state up-to-date
            //

            // This could be the first call that enables sharding - make sure we initialize the
            // sharding state for this shard.
            if ( ! shardingState.enabled() ) {
                if ( cmdObj["configdb"].type() != String ) {
                    errmsg = "sharding not enabled";
                    warning() << errmsg << endl;
                    return false;
                }
                string configdb = cmdObj["configdb"].String();
                ShardingState::initialize(configdb);
            }

            // Initialize our current shard name in the shard state if needed
            shardingState.gotShardName(shardName);

            ConnectionString configLoc = ConnectionString::parse(shardingState.getConfigServer(),
                                                                 errmsg);
            if (!configLoc.isValid()) {
                warning() << errmsg;
                return false;
            }

            log() << "received splitChunk request: " << cmdObj;

            //
            // 2. lock the collection's metadata and get highest version for the current shard
            //

            ScopedDistributedLock collLock(configLoc, ns);
            collLock.setLockMessage(str::stream() << "splitting chunk [" << minKey << ", " << maxKey
                                                  << ") in " << ns);

            if (!collLock.tryAcquire(&errmsg)) {

                errmsg = str::stream() << "could not acquire collection lock for " << ns
                                       << " to split chunk [" << minKey << "," << maxKey << ")"
                                       << causedBy(errmsg);

                warning() << errmsg;
                return false;
            }

            // Always check our version remotely
            ChunkVersion shardVersion;
            Status refreshStatus = shardingState.refreshMetadataNow(txn, ns, &shardVersion);

            if (!refreshStatus.isOK()) {

                errmsg = str::stream() << "splitChunk cannot split chunk " << "[" << minKey << ","
                                       << maxKey << ")" << causedBy(refreshStatus.reason());

                warning() << errmsg;
                return false;
            }

            if (shardVersion.majorVersion() == 0) {

                // It makes no sense to split if our version is zero and we have no chunks
                errmsg = str::stream() << "splitChunk cannot split chunk " << "[" << minKey << ","
                                       << maxKey << ")" << " with zero shard version";

                warning() << errmsg;
                return false;
            }

            // Get collection metadata
            const CollectionMetadataPtr collMetadata(shardingState.getCollectionMetadata(ns));
            // With nonzero shard version, we must have metadata
            invariant(NULL != collMetadata);

            ChunkVersion collVersion = collMetadata->getCollVersion();
            // With nonzero shard version, we must have a coll version >= our shard version
            invariant(collVersion >= shardVersion);

            ChunkType origChunk;
            if (!collMetadata->getNextChunk(min, &origChunk)
                || origChunk.getMin().woCompare(min) || origChunk.getMax().woCompare(max)) {

                // Our boundaries are different from those passed in
                errmsg = str::stream() << "splitChunk cannot find chunk "
                                       << "[" << minKey << "," << maxKey << ")"
                                       << " to split, the chunk boundaries may be stale";

                warning() << errmsg;
                return false;
            }

            log() << "splitChunk accepted at version " << shardVersion;

            //
            // 3. create the batch of updates to metadata ( the new chunks ) to be applied via 'applyOps' command
            //

            BSONObjBuilder logDetail;
            appendShortVersion(logDetail.subobjStart("before"), origChunk);
            LOG(1) << "before split on " << origChunk << endl;
            OwnedPointerVector<ChunkType> newChunks;

            ChunkVersion nextChunkVersion = collVersion;
            BSONObj startKey = min;
            splitKeys.push_back( max ); // makes it easier to have 'max' in the next loop. remove later.

            BSONObjBuilder cmdBuilder;
            BSONArrayBuilder updates( cmdBuilder.subarrayStart( "applyOps" ) );

            for ( vector<BSONObj>::const_iterator it = splitKeys.begin(); it != splitKeys.end(); ++it ) {
                BSONObj endKey = *it;

                if ( endKey.woCompare( startKey ) == 0) {
                    errmsg = str::stream() << "split on the lower bound of chunk "
                                           << "[" << min << ", " << max << ")"
                                           << " is not allowed";

                    warning() << errmsg << endl;
                    return false;
                }

                if (!isShardDocSizeValid(collMetadata->getKeyPattern(), endKey, &errmsg)) {
                    warning() << errmsg << endl;
                    return false;
                }

                // splits only update the 'minor' portion of version
                nextChunkVersion.incMinor();

                // build an update operation against the chunks collection of the config database with
                // upsert true
                BSONObjBuilder op;
                op.append( "op" , "u" );
                op.appendBool( "b" , true );
                op.append( "ns" , ChunkType::ConfigNS );

                // add the modified (new) chunk information as the update object
                BSONObjBuilder n( op.subobjStart( "o" ) );
                n.append(ChunkType::name(), Chunk::genID(ns, startKey));
                nextChunkVersion.addToBSON(n, ChunkType::DEPRECATED_lastmod());
                n.append(ChunkType::ns(), ns);
                n.append(ChunkType::min(), startKey);
                n.append(ChunkType::max(), endKey);
                n.append(ChunkType::shard(), shardName);
                n.done();

                // add the chunk's _id as the query part of the update statement
                BSONObjBuilder q( op.subobjStart( "o2" ) );
                q.append(ChunkType::name(), Chunk::genID(ns, startKey));
                q.done();

                updates.append( op.obj() );

                // remember this chunk info for logging later
                auto_ptr<ChunkType> chunk(new ChunkType());
                chunk->setMin(startKey);
                chunk->setMax(endKey);
                chunk->setVersion(nextChunkVersion);

                newChunks.push_back(chunk.release());

                startKey = endKey;
            }

            splitKeys.pop_back(); // 'max' was used as sentinel

            updates.done();

            {
                BSONArrayBuilder preCond( cmdBuilder.subarrayStart( "preCondition" ) );
                BSONObjBuilder b;
                b.append("ns", ChunkType::ConfigNS);
                b.append("q", BSON("query" << BSON(ChunkType::ns(ns)) <<
                                   "orderby" << BSON(ChunkType::DEPRECATED_lastmod() << -1)));
                {
                    BSONObjBuilder bb( b.subobjStart( "res" ) );
                    // TODO: For backwards compatibility, we can't yet require an epoch here
                    bb.appendTimestamp(ChunkType::DEPRECATED_lastmod(), collVersion.toLong());
                    bb.done();
                }
                preCond.append( b.obj() );
                preCond.done();
            }

            //
            // 4. apply the batch of updates to remote and local metadata
            //

            BSONObj cmd = cmdBuilder.obj();

            LOG(1) << "splitChunk update: " << cmd << endl;

            bool ok;
            BSONObj cmdResult;
            {
                ScopedDbConnection conn(shardingState.getConfigServer(), 30);
                ok = conn->runCommand( "config" , cmd , cmdResult );
                conn.done();
            }

            if ( ! ok ) {
                stringstream ss;
                ss << "saving chunks failed.  cmd: " << cmd << " result: " << cmdResult;
                error() << ss.str() << endl;
                msgasserted( 13593 , ss.str() );
            }

            //
            // Install chunk metadata with knowledge about newly split chunks in this shard's state
            //
            
            {
                Lock::DBWrite writeLk(txn->lockState(), ns);

                // NOTE: The newShardVersion resulting from this split is higher than any other
                // chunk version, so it's also implicitly the newCollVersion
                ChunkVersion newShardVersion = collVersion;

                // Increment the minor version once, shardingState.splitChunk increments once per
                // split point (resulting in the correct final shard/collection version)
                // TODO: Revisit this interface, it's a bit clunky
                newShardVersion.incMinor();

                shardingState.splitChunk(txn, ns, min, max, splitKeys, newShardVersion);
            }

            //
            // 5. logChanges
            //

            // single splits are logged different than multisplits
            if ( newChunks.size() == 2 ) {
                appendShortVersion(logDetail.subobjStart("left"), *newChunks[0]);
                appendShortVersion(logDetail.subobjStart("right"), *newChunks[1]);
                configServer.logChange( "split" , ns , logDetail.obj() );
            }
            else {
                BSONObj beforeDetailObj = logDetail.obj();
                BSONObj firstDetailObj = beforeDetailObj.getOwned();
                const int newChunksSize = newChunks.size();

                for ( int i=0; i < newChunksSize; i++ ) {
                    BSONObjBuilder chunkDetail;
                    chunkDetail.appendElements( beforeDetailObj );
                    chunkDetail.append( "number", i+1 );
                    chunkDetail.append( "of" , newChunksSize );
                    appendShortVersion(chunkDetail.subobjStart("chunk"), *newChunks[i]);
                    configServer.logChange( "multi-split" , ns , chunkDetail.obj() );
                }
            }

            dassert(newChunks.size() > 1);

            {
                Client::ReadContext ctx(txn, ns);
                Collection* collection = ctx.ctx().db()->getCollection(txn, ns);
                invariant(collection);

                // Allow multiKey based on the invariant that shard keys must be
                // single-valued. Therefore, any multi-key index prefixed by shard
                // key cannot be multikey over the shard key fields.
                IndexDescriptor *idx =
                    collection->getIndexCatalog()->findIndexByPrefix(txn, keyPattern, false);

                if (idx == NULL) {
                    return true;
                }

                const ChunkType* chunk = newChunks.vector().back();
                KeyPattern kp(idx->keyPattern());
                BSONObj newmin = Helpers::toKeyFormat(kp.extendRangeBound(chunk->getMin(), false));
                BSONObj newmax = Helpers::toKeyFormat(kp.extendRangeBound(chunk->getMax(), false));

                auto_ptr<PlanExecutor> exec(
                        InternalPlanner::indexScan(txn, collection, idx, newmin, newmax, false));

                // check if exactly one document found
                if (PlanExecutor::ADVANCED == exec->getNext(NULL, NULL)) {
                    if (PlanExecutor::IS_EOF == exec->getNext(NULL, NULL)) {
                        result.append("shouldMigrate",
                                      BSON("min" << chunk->getMin() << "max" << chunk->getMax()));
                    }
                }
            }

            return true;
        }

    private:

        /**
         * Append min, max and version information from chunk to the buffer.
         */
        static void appendShortVersion(BufBuilder& b, const ChunkType& chunk) {
            BSONObjBuilder bb(b);
            bb.append(ChunkType::min(), chunk.getMin());
            bb.append(ChunkType::max(), chunk.getMax());
            if (chunk.isVersionSet())
                chunk.getVersion().addToBSON(bb, ChunkType::DEPRECATED_lastmod());
            bb.done();
        }

    } cmdSplitChunk;

}  // namespace mongo
