MongoDB Error Codes
==========




src/mongo/bson/bson-inl.h
----
* 10065 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L198) 
* 10313 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L571) Insufficient bytes to calculate element size
* 10314 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L575) Insufficient bytes to calculate element size
* 10315 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L580) Insufficient bytes to calculate element size
* 10316 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L585) Insufficient bytes to calculate element size
* 10317 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L589) Insufficient bytes to calculate element size
* 10318 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L595) Invalid regex string
* 10319 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L605) Invalid regex options string
* 10320 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L679) 
* 10321 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L516) 
* 10322 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L521) Invalid CodeWScope size
* 10323 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L523) Invalid CodeWScope string size
* 10324 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L524) Invalid CodeWScope string size
* 10325 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L527) Invalid CodeWScope size
* 10326 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L529) Invalid CodeWScope object size
* 10327 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L478) Object does not end with EOO
* 10328 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L480) Invalid element size
* 10329 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L481) Element too large
* 10330 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L483) Element extends past end of object
* 10331 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L488) EOO Before end of object
* 10334 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L237) 
* 13655 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L613) 


src/mongo/bson/bson_db.h
----
* 10062 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson_db.h#L60) not code


src/mongo/bson/bsonelement.h
----
* 10063 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonelement.h#L365) not a dbref
* 10064 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonelement.h#L370) not a dbref
* 10333 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonelement.h#L395) Invalid field name
* 13111 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonelement.h#L432) 
* 13118 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonelement.h#L437) unexpected or missing type value in BSON object


src/mongo/bson/bsonobjbuilder.h
----
* 10335 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonobjbuilder.h#L548) builder does not own memory
* 10336 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonobjbuilder.h#L623) No subobject started
* 13048 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonobjbuilder.h#L774) can't append to array using string field name [" + name.data() + "]
* 15891 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonobjbuilder.h#L782) can't backfill array to larger than 1,500,000 elements


src/mongo/bson/ordering.h
----
* 13103 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/ordering.h#L64) too many compound keys


src/mongo/bson/util/builder.h
----
* 10000 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/util/builder.h#L97) out of memory BufBuilder
* 13548 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/util/builder.h#L209) 
* 15912 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/util/builder.h#L72) out of memory StackAllocator::Realloc
* 15913 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/util/builder.h#L122) out of memory BufBuilder::reset
* 16070 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/util/builder.h#L213) out of memory BufBuilder::grow_reallocate


src/mongo/client/clientAndShell.cpp
----
* 10256 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/clientAndShell.cpp#L73) no createDirectClient in clientOnly


src/mongo/client/connpool.cpp
----
* 13071 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/connpool.cpp#L196) invalid hostname [" + host + "]
* 13328 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/connpool.cpp#L176) : connect failed " + url.toString() + " : 


src/mongo/client/connpool.h
----
* 11004 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/connpool.h#L230) connection was returned to the pool already
* 11005 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/connpool.h#L236) connection was returned to the pool already
* 13102 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/connpool.h#L242) connection was returned to the pool already


src/mongo/client/dbclient.cpp
----
* 10005 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L532) listdatabases failed" , runCommand( "admin" , BSON( "listDatabases
* 10006 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L533) listDatabases.databases not array" , info["databases
* 10007 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L887) dropIndex failed
* 10008 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L894) dropIndexes failed
* 10276 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L594) DBClientBase::findN: transport error: " << getServerAddress() << " ns: " << ns << " query: 
* 10278 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L1028) dbclient error communicating with server: 
* 10337 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L980) object not valid
* 11010 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L307) count fails:
* 13386 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L771) socket error for mapping query
* 13421 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L110) trying to connect to invalid ConnectionString
* 16090 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L743) socket error for mapping query


src/mongo/client/dbclient_rs.cpp
----
* 10009 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L240) ReplicaSetMonitor no master found for set: 
* 13610 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L185) ConfigChangeHook already specified
* 13639 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L672) can't connect to new replica set master [" << _masterHost.toString() << "] err: 
* 13642 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L94) need at least 1 node for a replica set
* 15899 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L292) No suitable member found for slaveOk query in replica set: 


src/mongo/client/dbclientcursor.cpp
----
* 13127 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.cpp#L169) getMore: cursor didn't exist on server, possible restart or timeout?
* 13422 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.cpp#L223) DBClientCursor next() called but more() is false
* 14821 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.cpp#L289) No client or lazy client specified, cannot store multi-host connection.
* 15875 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.cpp#L81) DBClientCursor::initLazy called on a client that doesn't support lazy


src/mongo/client/dbclientcursor.h
----
* 13106 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.h#L81) 
* 13348 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.h#L224) connection died
* 13383 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.h#L241) BatchIterator empty


src/mongo/client/dbclientinterface.h
----
* 10011 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientinterface.h#L569) no collection name
* 9000 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientinterface.h#L890) 


src/mongo/client/distlock.cpp
----
* 14023 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/distlock.cpp#L608) remote time in cluster " << _conn.toString() << " is now skewed, cannot force lock.
* 16060 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/distlock.cpp#L126) cannot query locks collection on config server 


src/mongo/client/distlock_test.cpp
----
* 13678 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/distlock_test.cpp#L382) Could not communicate with server " << server.toString() << " in cluster " << cluster.toString() << " to change skew by 


src/mongo/client/gridfs.cpp
----
* 10012 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L95) file doesn't exist" , fileName == "-
* 10013 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L102) error opening file
* 10014 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L215) chunk is empty!
* 10015 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L247) doesn't exists
* 13296 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L69) invalid chunk size is specified
* 13325 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L241) couldn't open file: 
* 9008 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L141) filemd5 failed


src/mongo/client/parallel.cpp
----
* 10017 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L101) cursor already done
* 10018 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L413) no more items
* 10019 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L1482) no more elements
* 13431 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L500) have to have sort key in projection and removing it
* 13633 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L131) error querying server: 
* 14812 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L1553) Error running command on server: 
* 14813 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L1554) Command returned nothing
* 15986 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L738) too many retries in total
* 15987 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L859) could not fully initialize cursor on shard " << shard.toString() << ", current connection state is 
* 15988 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L990) error querying server
* 15989 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L704) database not found for parallel cursor request


src/mongo/client/syncclusterconnection.cpp
----
* 10022 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L255) SyncClusterConnection::getMore not supported yet
* 10023 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L277) SyncClusterConnection bulk insert not implemented
* 13053 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L394) help failed: " << info , _commandOnActive( "admin" , BSON( name << "1" << "help
* 13054 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L218) write $cmd not supported in SyncClusterConnection::query for:
* 13104 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L173) SyncClusterConnection::findOne prepare failed: 
* 13105 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L191) 
* 13119 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L262) SyncClusterConnection::insert obj has to have an _id: 
* 13120 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L295) SyncClusterConnection::update upsert query needs _id" , query.obj["_id
* 13397 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L370) SyncClusterConnection::say prepare failed: 
* 15848 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L203) sync cluster of sync clusters?
* 8001 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L138) SyncClusterConnection write op failed: 
* 8002 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L251) all servers down!
* 8003 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L267) SyncClusterConnection::insert prepare failed: 
* 8004 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L53) SyncClusterConnection needs 3 servers
* 8005 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L301) SyncClusterConnection::udpate prepare failed: 
* 8006 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L344) SyncClusterConnection::call can only be used directly for dbQuery
* 8007 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L348) SyncClusterConnection::call can't handle $cmd" , strstr( d.getns(), "$cmd
* 8008 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L364) all servers down!
* 8020 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L283) SyncClusterConnection::remove prepare failed: 


src/mongo/db/btree.cpp
----
* 10281 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/btree.cpp#L143) assert is misdefined
* 10282 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/btree.cpp#L324) n==0 in btree popBack()
* 10283 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/btree.cpp#L331) rchild not null in btree popBack()
* 10285 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/btree.cpp#L1771) _insert: reuse key but lchild is not null
* 10286 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/btree.cpp#L1772) _insert: reuse key but rchild is not null
* 10287 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/btree.cpp#L84) btree: key+recloc already in index
* 15898 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/btree.cpp#L42) error in index possibly corruption consider repairing 


src/mongo/db/btree.h
----
* 13000 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/btree.h#L361) invalid keyNode: " +  BSON( "i" << i << "n


src/mongo/db/btreebuilder.cpp
----
* 10288 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/btreebuilder.cpp#L76) bad key order in BtreeBuilder - server internal error


src/mongo/db/btreecursor.cpp
----
* 14800 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/btreecursor.cpp#L264) unsupported index version 
* 14801 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/btreecursor.cpp#L280) unsupported index version 
* 15850 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/btreecursor.cpp#L56) keyAt bucket deleted


src/mongo/db/cap.cpp
----
* 10345 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cap.cpp#L258) passes >= maxPasses in capped collection alloc
* 13415 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cap.cpp#L344) emptying the collection is not allowed
* 13424 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cap.cpp#L411) collection must be capped
* 13425 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cap.cpp#L412) background index build in progress
* 13426 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cap.cpp#L413) indexes present


src/mongo/db/client.cpp
----
* 10057 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/client.cpp#L304) 
* 14031 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/client.cpp#L280) Can't take a write lock while out of disk space
* 15928 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/client.cpp#L231) can't open a database from a nested read lock 
* 15929 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/client.cpp#L329) client access to index backing namespace prohibited
* 16107 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/client.cpp#L286) Don't have a lock on: 


src/mongo/db/client.h
----
* 12600 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/client.h#L265) releaseAndWriteLock: unlock_shared failed, probably recursive


src/mongo/db/clientcursor.cpp
----
* 16089 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/clientcursor.cpp#L748) 


src/mongo/db/clientcursor.h
----
* 12051 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/clientcursor.h#L111) clientcursor already in use? driver problem?
* 12521 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/clientcursor.h#L332) internal error: use of an unlocked ClientCursor


src/mongo/db/cloner.cpp
----
* 10024 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L93) bad ns field for index during dbcopy
* 10025 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L95) bad ns field for index during dbcopy [2]
* 10026 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L665) source namespace does not exist
* 10027 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L675) target namespace exists", cmdObj["dropTarget
* 10289 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L287) useReplAuth is not written to replication log
* 10290 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L363) 
* 13008 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L616) must call copydbgetnonce first
* 15908 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L241) 
* 15967 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L654) invalid collection name: 


src/mongo/db/cmdline.cpp
----
* 10033 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cmdline.cpp#L385) logpath has to be non-zero


src/mongo/db/commands.cpp
----
* 15962 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands.cpp#L36) need to specify namespace
* 15966 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands.cpp#L37) dbname not ok in Command::parseNsFullyQualified: " << dbname , dbname == nss.db || dbname == "admin


src/mongo/db/commands/distinct.cpp
----
* 10044 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/distinct.cpp#L117) distinct too big, 16mb cap


src/mongo/db/commands/document_source_cursor.cpp
----
* 16028 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/document_source_cursor.cpp#L67) 


src/mongo/db/commands/find_and_modify.cpp
----
* 12515 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/find_and_modify.cpp#L94) can't remove and update", cmdObj["update
* 12516 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/find_and_modify.cpp#L126) must specify remove or update
* 13329 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/find_and_modify.cpp#L71) upsert mode requires update field
* 13330 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/find_and_modify.cpp#L72) upsert mode requires query field


src/mongo/db/commands/group.cpp
----
* 10041 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/group.cpp#L42) invoke failed in $keyf: 
* 10042 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/group.cpp#L44) return of $key has to be an object
* 10043 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/group.cpp#L123) group() can't handle more than 20000 unique keys
* 9010 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/group.cpp#L129) reduce invoke failed: 


src/mongo/db/commands/isself.cpp
----
* 13469 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/isself.cpp#L48) getifaddrs failure: 
* 13470 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/isself.cpp#L63) getnameinfo() failed: 
* 13472 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/isself.cpp#L109) getnameinfo() failed: 


src/mongo/db/commands/mr.cpp
----
* 10074 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L155) need values
* 10075 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L196) reduce -> multiple not supported yet
* 10076 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L486) rename failed: 
* 10077 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L951) fast_emit takes 2 args
* 13069 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L952) an emit can't be more than half max bson size
* 13070 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L176) value too large to reduce
* 13522 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L263) unknown out specifier [" << t << "]
* 13598 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L55) couldn't compile code for: 
* 13602 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L235) outType is no longer a valid option" , cmdObj["outType
* 13604 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L433) too much data for in memory map/reduce
* 13606 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L277) 'out' has to be a string or an object
* 13608 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L317) query has to be blank or an Object
* 13609 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L324) sort has to be blank or an Object
* 13630 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L363) userCreateNS failed for mr tempLong ns: " << _config.tempLong << " err: 
* 13631 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L348) userCreateNS failed for mr incLong ns: " << _config.incLong << " err: 
* 15876 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L1029) could not create cursor over " << config.ns << " to hold data while prepping m/r
* 15877 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L1031) could not create m/r holding client cursor over 
* 15895 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L273) nonAtomic option cannot be used with this output type
* 15921 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L417) splitVector failed: 
* 16052 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L1080) could not create cursor over " << config.ns << " for query : " << config.filter << " sort : 
* 16053 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L1082) could not create client cursor over " << config.ns << " for query : " << config.filter << " sort : 
* 16054 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L282) shardedFirstPass should only use replace outType
* 9014 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L73) map invoke failed: 


src/mongo/db/commands/pipeline.cpp
----
* 15942 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/pipeline.cpp#L145) pipeline element 
* 16029 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/pipeline.cpp#L399) 


src/mongo/db/compact.cpp
----
* 13660 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/compact.cpp#L296) namespace " << ns << " does not exist
* 13661 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/compact.cpp#L297) cannot compact capped collection
* 14024 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/compact.cpp#L110) compact error out of space during compaction
* 14025 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/compact.cpp#L227) compact error no space available to allocate
* 14027 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/compact.cpp#L288) can't compact a system namespace", !str::contains(ns, ".system.
* 14028 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/compact.cpp#L287) bad ns


src/mongo/db/curop.h
----
* 11600 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/curop.h#L276) interrupted at shutdown
* 11601 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/curop.h#L278) operation was interrupted
* 12601 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/curop.h#L202) CurOp not marked done yet


src/mongo/db/cursor.h
----
* 13285 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cursor.h#L193) manual matcher config not allowed


src/mongo/db/d_concurrency.cpp
----
* 16098 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L601) can't dblock:" << db << " when local or admin is already locked
* 16099 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L783) internal error tried to lock two databases at the same time. old:" << ls.otherName << " new:
* 16100 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L788) can't dblock:" << db << " when local or admin is already locked
* 16101 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L875) expected write lock
* 16102 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L882) expected read lock
* 16103 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L232) can't lock_R, threadState=
* 16104 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L355) expected to be read locked for 
* 16105 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L361) expected to be write locked for 
* 16106 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L596) internal error tried to lock two databases at the same time. old:" << ls.otherName << " new:
* 16114 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L214) 
* 16115 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L372) 
* 16116 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L394) 
* 16117 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L395) 
* 16118 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L396) 
* 16119 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L408) 
* 16120 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L409) 
* 16121 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L415) 
* 16122 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L417) 
* 16123 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L418) 
* 16124 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L419) 
* 16125 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L423) 
* 16126 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L425) 
* 16127 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L430) 
* 16128 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L432) 
* 16129 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L436) 
* 16130 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L438) 
* 16131 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L560) 
* 16132 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L568) 
* 16133 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L582) 
* 16134 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L614) 
* 16135 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L801) 


src/mongo/db/d_concurrency.h
----
* 13142 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.h#L169) timeout getting readlock


src/mongo/db/database.cpp
----
* 10028 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/database.cpp#L69) db name is empty
* 10029 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/database.cpp#L71) bad db name [1]
* 10030 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/database.cpp#L72) bad db name [2]
* 10031 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/database.cpp#L73) bad char(s) in db name
* 10032 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/database.cpp#L70) db name too long
* 10295 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/database.cpp#L233) getFile(): bad file number value (corrupt db?): run repair
* 12501 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/database.cpp#L317) quota exceeded
* 14810 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/database.cpp#L332) couldn't allocate space (suitableFile)
* 15924 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/database.cpp#L167) getFile(): bad file number value " << n << " (corrupt db?): run repair
* 15927 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/database.cpp#L431) can't open database in a read lock. if db was just closed, consider retrying the query. might otherwise indicate an internal error


src/mongo/db/databaseholder.h
----
* 13074 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/databaseholder.h#L90) db name can't be empty
* 13075 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/databaseholder.h#L93) db name can't be empty
* 13280 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/databaseholder.h#L84) invalid db name: 


src/mongo/db/db.cpp
----
* 10296 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/db.cpp#L471) 
* 10297 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/db.cpp#L1320) Couldn't register Windows Ctrl-C handler
* 12590 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/db.cpp#L476) 
* 14026 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/db.cpp#L294) 


src/mongo/db/db.h
----
* 10298 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/db.h#L41) can't temprelease nested lock


src/mongo/db/dbcommands.cpp
----
* 10039 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L809) can't drop collection with reserved $ character in name
* 10040 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L1135) chunks out of order
* 10301 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L1473) source collection " + fromNs + " does not exist
* 13049 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L1610) godinsert must specify a collection
* 13281 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L1154) File deleted during filemd5 command
* 13416 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L1746) captrunc must specify a collection
* 13417 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L1754) captrunc collection not found or empty
* 13418 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L1756) captrunc invalid n
* 13428 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L1773) emptycapped must specify a collection
* 13429 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L1776) emptycapped no such collection
* 14832 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L872) specify size:<n> when capped is true", !cmdObj["capped"].trueValue() || cmdObj["size"].isNumber() || cmdObj.hasField("$nExtents
* 15880 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L613) no ram log for warnings?
* 15888 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L869) must pass name of collection to create


src/mongo/db/dbcommands_admin.cpp
----
* 12032 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands_admin.cpp#L446) fsync: sync option must be true when using lock
* 12034 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands_admin.cpp#L445) fsync: can't lock while an unlock is pending


src/mongo/db/dbcommands_generic.cpp
----
* 10038 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands_generic.cpp#L408) forced error


src/mongo/db/dbeval.cpp
----
* 10046 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbeval.cpp#L42) eval needs Code
* 12598 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbeval.cpp#L123) $eval reads unauthorized


src/mongo/db/dbhelpers.cpp
----
* 13430 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbhelpers.cpp#L127) no _id index


src/mongo/db/dbmessage.h
----
* 10304 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbmessage.h#L199) Client Error: Remaining data too small for BSON object
* 10305 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbmessage.h#L201) Client Error: Invalid object size
* 10306 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbmessage.h#L202) Client Error: Next object larger than space left in message
* 10307 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbmessage.h#L205) Client Error: bad object in message
* 13066 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbmessage.h#L197) Message contains no documents


src/mongo/db/dbwebserver.cpp
----
* 13453 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbwebserver.cpp#L170) server not started with --jsonp


src/mongo/db/dur.cpp
----
* 13599 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur.cpp#L428) Written data does not match in-memory view. Missing WriteIntent?
* 13616 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur.cpp#L197) can't disable durability with pending writes
* 16110 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur.cpp#L274) 
* 16111 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur.cpp#L275) 


src/mongo/db/dur_journal.cpp
----
* 13611 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_journal.cpp#L539) can't read lsn file in journal directory : 
* 13614 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_journal.cpp#L506) unexpected version number of lsn file in journal/ directory got: 
* 15926 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_journal.cpp#L354) Insufficient free space for journals


src/mongo/db/dur_recover.cpp
----
* 13531 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L79) unexpected files in journal directory " << dir.string() << " : 
* 13532 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L86) 
* 13533 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L162) problem processing journal file during recovery
* 13535 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L467) recover abrupt journal file end
* 13536 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L394) journal version number mismatch 
* 13537 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L385) 
* 13544 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L449) recover error couldn't open 
* 13545 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L474) --durOptions " << (int) CmdLine::DurScanOnly << " (scan only) specified
* 13594 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L358) journal checksum doesn't match
* 13622 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L255) Trying to write past end of file in WRITETODATAFILES
* 15874 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L114) couldn't uncompress journal section


src/mongo/db/durop.cpp
----
* 13546 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/durop.cpp#L53) journal recover: unrecognized opcode in journal 
* 13547 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/durop.cpp#L144) recover couldn't create file 
* 13628 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/durop.cpp#L158) recover failure writing file 


src/mongo/db/extsort.cpp
----
* 10048 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/extsort.cpp#L93) already sorted
* 10049 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/extsort.cpp#L118) sorted already
* 10050 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/extsort.cpp#L139) bad
* 10308 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/extsort.cpp#L250) mmap failed


src/mongo/db/extsort.h
----
* 10052 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/extsort.h#L105) not sorted


src/mongo/db/geo/2d.cpp
----
* 13022 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L120) can't have 2 geo field
* 13023 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L121) 2d has to be first in index
* 13024 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L130) no geo field specified
* 13026 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L333) geo values have to be numbers: 
* 13027 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L356) point not in interval of [ " << _min << ", " << _max << " )
* 13028 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L134) bits in geo index must be between 1 and 32
* 13042 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L2859) missing geo field (" + _geo + ") in : 
* 13046 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L2915) 'near' param missing/invalid", !cmdObj["near
* 13057 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L2825) $within has to take an object or array
* 13058 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L2849) unknown $within information : " << context << ", a shape must be specified.
* 13059 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L2835) $center has to take an object or array
* 13060 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L2496) $center needs 2 fields (middle,max distance)
* 13061 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L2510) need a max distance >= 0 
* 13063 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L2614) $box needs 2 fields (bottomLeft,topRight)
* 13064 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L2626) need an area > 0 
* 13065 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L2840) $box has to take an object or array
* 13067 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L328) geo field is empty
* 13068 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L330) geo field only has 1 element
* 13460 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L2533) invalid $center query type: 
* 13461 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L2521) Spherical MaxDistance > PI. Are you sure you are using radians?
* 13462 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L2528) Spherical distance would require wrapping, which isn't implemented yet
* 13464 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L2794) invalid $near search type: 
* 13654 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L249) location object expected, location array not in correct format
* 13656 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L2501) the first field of $center object must be a location object
* 14029 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L2845) $polygon has to take an object or array
* 14030 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/2d.cpp#L2709) polygon must be defined by three points or more


src/mongo/db/geo/core.h
----
* 13047 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/core.h#L106) wrong type for geo index. if you're using a pre-release version, need to rebuild index
* 14808 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/core.h#L509) point " << p.toString() << " must be in earth-like bounds of long : [-180, 180), lat : [-90, 90] 


src/mongo/db/geo/haystack.cpp
----
* 13314 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/haystack.cpp#L90) can't have 2 geo fields
* 13315 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/haystack.cpp#L91) 2d has to be first in index
* 13316 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/haystack.cpp#L100) no geo field specified
* 13317 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/haystack.cpp#L101) no other fields specified
* 13318 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/haystack.cpp#L299) near needs to be an array
* 13319 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/haystack.cpp#L300) maxDistance needs a number
* 13320 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/haystack.cpp#L301) search needs to be an object
* 13321 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/haystack.cpp#L81) need bucketSize
* 13322 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/haystack.cpp#L107) not a number
* 13323 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/haystack.cpp#L142) latlng not an array
* 13326 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/haystack.cpp#L102) quadrant search can only have 1 other field for now


src/mongo/db/index.cpp
----
* 10096 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.cpp#L317) invalid ns to index
* 10097 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.cpp#L318) bad table to index name on add index attempt current db: " << cc().database()->name << "  source: 
* 10098 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.cpp#L325) 
* 11001 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.cpp#L98) 
* 12504 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.cpp#L332) 
* 12505 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.cpp#L362) 
* 12523 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.cpp#L313) no index name specified
* 12524 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.cpp#L322) index key pattern too large
* 12588 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.cpp#L368) cannot add index with a background operation in progress
* 14803 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.cpp#L403) this version of mongod cannot build new indexes of version number 


src/mongo/db/index.h
----
* 14802 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.h#L167) index v field should be Integer type


src/mongo/db/indexkey.cpp
----
* 13007 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/indexkey.cpp#L66) can only have 1 index plugin / bad index key pattern
* 13529 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/indexkey.cpp#L83) sparse only works for single field keys
* 15855 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/indexkey.cpp#L300) Ambiguous field name found in array (do not use numeric field names in embedded elements in an array), field: '" << arrField.fieldName() << "' for array: 
* 15869 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/indexkey.cpp#L416) Invalid index version for key generation.


src/mongo/db/instance.cpp
----
* 10054 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L560) not master
* 10055 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L543) update object too large
* 10056 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L594) not master
* 10058 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L773) not master
* 10059 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L720) object to insert too large
* 10309 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1092) Unable to create/open lock file: " << name << ' ' << errnoWithDescription() << " Is a mongod instance already running?
* 10310 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1097) Unable to lock file: " + name + ". Is a mongod instance already running?
* 10332 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L109) Expected CurrentTime type
* 12596 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1149) old lock file
* 13004 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L488) sent negative cursors to kill: 
* 13073 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L661) shutting down
* 13342 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1167) Unable to truncate lock file
* 13455 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L994) dbexit timed out getting lock
* 13511 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L726) document to insert can't have $ fields
* 13597 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1159) can't start without --journal enabled when journal/ files are present
* 13618 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1184) can't start without --journal enabled when journal/ files are present
* 13625 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1163) Unable to truncate lock file
* 13627 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1086) Unable to create/open lock file: " << name << ' ' << m << ". Is a mongod instance already running?
* 13637 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L878) count failed in DBDirectClient: 
* 13658 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L487) bad kill cursors size: 
* 13659 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L486) sent 0 cursors to kill


src/mongo/db/jsobj.cpp
----
* 10060 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/jsobj.cpp#L535) woSortOrder needs a non-empty sortKey
* 10061 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/jsobj.cpp#L1132) type not supported for appendMinElementForType
* 10311 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/jsobj.cpp#L93) 
* 10312 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/jsobj.cpp#L251) 
* 12579 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/jsobj.cpp#L854) unhandled cases in BSONObj okForStorage
* 14853 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/jsobj.cpp#L1185) type not supported for appendMaxElementForType


src/mongo/db/json.cpp
----
* 10338 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/json.cpp#L231) Invalid use of reserved field name: 
* 10339 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/json.cpp#L404) Badly formatted bindata
* 10340 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/json.cpp#L640) Failure parsing JSON string near: 


src/mongo/db/lasterror.cpp
----
* 13649 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/lasterror.cpp#L95) no operation yet


src/mongo/db/matcher.cpp
----
* 10066 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L361) $where may only appear once in query
* 10067 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L362) $where query, but no script engine
* 10068 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L225) invalid operator: 
* 10069 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L311) BUG - can't operator for: 
* 10070 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L963) $where compile error
* 10071 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L978) 
* 10072 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L982) unknown error in invocation of $where function
* 10073 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L106) mod can't be 0
* 10341 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L88) scope has to be created first!
* 10342 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L1282) pcre not compiled with utf8 support
* 12517 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L113) $elemMatch needs an Object
* 13020 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L165) with $all, can't mix $elemMatch and others
* 13029 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L302) can't use $not with $options, use BSON regex type instead
* 13030 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L423) $not cannot be empty
* 13031 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L433) invalid use of $not
* 13032 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L291) can't use $not with $regex, use BSON regex type instead
* 13086 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L317) $and/$or/$nor must be a nonempty array
* 13087 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L321) $and/$or/$nor match element must be an object
* 13089 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L363) no current client needed for $where
* 13276 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L250) $in needs an array
* 13277 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L261) $nin needs an array
* 13629 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L382) can't have undefined in a query expression
* 14844 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L460) $atomic specifier must be a top level field
* 15882 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L157) $elemMatch not allowed within $in
* 15902 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L360) $where expression has an unexpected type


src/mongo/db/mongommf.cpp
----
* 13520 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/mongommf.cpp#L267) MongoMMF only supports filenames in a certain format 
* 13636 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/mongommf.cpp#L296) file " << filename() << " open/create failed in createPrivateMap (look in log for more information)
* 16112 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/mongommf.cpp#L151) 


src/mongo/db/namespace-inl.h
----
* 10080 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace-inl.h#L35) ns name too long, max size is 128
* 10348 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace-inl.h#L45) $extra: ns name too long


src/mongo/db/namespace_details-inl.h
----
* 10349 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace_details-inl.h#L55) E12000 idxNo fails
* 13283 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace_details-inl.h#L33) Missing Extra
* 14045 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace_details-inl.h#L34) missing Extra
* 14823 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace_details-inl.h#L41) missing extra
* 14824 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace_details-inl.h#L42) missing Extra


src/mongo/db/namespace_details.cpp
----
* 10079 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace_details.cpp#L172) bad .ns file length, cannot open database
* 10081 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace_details.cpp#L483) too many namespaces/collections
* 10082 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace_details.cpp#L498) allocExtra: too many namespaces/collections
* 10343 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace_details.cpp#L179) bad lenForNewNsFiles
* 10346 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace_details.cpp#L561) not implemented
* 10350 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace_details.cpp#L493) allocExtra: base ns missing?
* 10351 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace_details.cpp#L494) allocExtra: extra already exists


src/mongo/db/namespacestring.h
----
* 10078 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespacestring.h#L133) nsToDatabase: ns too long
* 10088 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespacestring.h#L144) nsToDatabase: ns too long


src/mongo/db/nonce.cpp
----
* 10352 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/nonce.cpp#L33) Security is a singleton class
* 10353 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/nonce.cpp#L44) can't open dev/urandom: 
* 10354 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/nonce.cpp#L53) md5 unit test fails
* 10355 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/nonce.cpp#L62) devrandom failed


src/mongo/db/oplog.cpp
----
* 13044 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplog.cpp#L524) no ts field in query
* 13257 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplog.cpp#L349) 
* 13288 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplog.cpp#L52) replSet error write op to db before replSet initialized", str::startsWith(ns, "local.
* 13312 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplog.cpp#L141) replSet error : logOp() but not primary?
* 13347 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplog.cpp#L179) local.oplog.rs missing. did you drop it? if so restart server
* 13389 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplog.cpp#L71) local.oplog.rs missing. did you drop it? if so restart server
* 14038 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplog.cpp#L468) invalid _findingStartMode
* 14825 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplog.cpp#L838) error in applyOperation : unknown opType 
* 15916 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplog.cpp#L670) Can no longer connect to initial sync source: 
* 15917 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplog.cpp#L704) Got bad disk location when attempting to insert


src/mongo/db/oplogreader.h
----
* 15910 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplogreader.h#L82) Doesn't have cursor for reading oplog
* 15911 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplogreader.h#L87) Doesn't have cursor for reading oplog


src/mongo/db/ops/delete.cpp
----
* 10100 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/delete.cpp#L42) cannot delete from collection with reserved $ in name
* 10101 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/delete.cpp#L50) can't remove from a capped collection
* 12050 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/delete.cpp#L38) cannot delete from system namespace


src/mongo/db/ops/query.cpp
----
* 10110 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/query.cpp#L845) bad query object
* 13051 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/query.cpp#L855) tailable cursor requested on non capped collection
* 13052 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/query.cpp#L861) only {$natural:1} order allowed for tailable cursor
* 13530 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/query.cpp#L828) bad or malformed command request?
* 14833 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/query.cpp#L102) auth error


src/mongo/db/ops/query.h
----
* 16083 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/query.h#L45) reserve 16083


src/mongo/db/ops/update.cpp
----
* 10131 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L123) $push can only be applied to an array
* 10132 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L194) $pushAll can only be applied to an array
* 10133 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L195) $pushAll has to be passed an array
* 10134 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L219) $pull/$pullAll can only be applied to an array
* 10135 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L252) $pop can only be applied to an array
* 10136 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L288) $bit needs an object
* 10137 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L289) $bit can only be applied to numbers
* 10138 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L290) $bit cannot update a value of type double
* 10139 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L298) $bit field must be number
* 10140 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L407) Cannot apply $inc modifier to non-number
* 10141 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L429) Cannot apply $push/$pushAll modifier to non-array
* 10142 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L435) Cannot apply $pull/$pullAll modifier to non-array
* 10143 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L456) Cannot apply $pop modifier to non-array
* 10145 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L694) LEFT_SUBFIELD only supports Object: " << field << " not: 
* 10147 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L825) Invalid modifier specified: 
* 10148 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L838) Mod on _id not allowed", strcmp( fieldName, "_id
* 10149 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L839) Invalid mod field name, may not end in a period
* 10150 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L840) Field name duplication not allowed with modifiers
* 10151 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L841) have conflicting mods in update
* 10152 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L842) Modifier $inc allowed for numbers only
* 10153 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L843) Modifier $pushAll/pullAll allowed for arrays only
* 10154 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L919) Modifiers and non-modifiers cannot be mixed
* 10155 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L1334) cannot update reserved $ collection
* 10156 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L1337) cannot update system collection: " << ns << " q: " << patternOrig << " u: 
* 10157 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L1173) multi-update requires all modified objects to have an _id
* 10158 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L1284) multi update only works with $ operators
* 10159 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L1312) multi update only works with $ operators
* 10399 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L736) ModSet::createNewFromMods - RIGHT_SUBFIELD should be impossible
* 10400 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L739) unhandled case
* 12522 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L924) $ operator made object too large
* 12591 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L462) Cannot apply $addToSet modifier to non-array
* 12592 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L140) $addToSet can only be applied to an array
* 13478 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L602) can't apply mod in place - shouldn't have gotten here
* 13479 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L850) invalid mod field name, target may not be empty
* 13480 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L851) invalid mod field name, source may not begin or end in period
* 13481 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L852) invalid mod field name, target may not begin or end in period
* 13482 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L853) $rename affecting _id not allowed
* 13483 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L854) $rename affecting _id not allowed
* 13484 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L855) field name duplication not allowed with $rename target
* 13485 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L856) conflicting mods not allowed with $rename target
* 13486 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L857) $rename target may not be a parent of source
* 13487 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L858) $rename source may not be dynamic array", strstr( fieldName , ".$
* 13488 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L859) $rename target may not be dynamic array", strstr( target , ".$
* 13489 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L379) $rename source field invalid
* 13490 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L390) $rename target field invalid
* 13494 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L846) $rename target must be a string
* 13495 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L848) $rename source must differ from target
* 13496 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L849) invalid mod field name, source may not be empty
* 15896 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L837) Modified field name may not start with $
* 16069 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L714) ModSet::createNewFromMods - 
* 9016 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L314) unknown $bit operation: 
* 9017 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L337) Mod::apply can't handle type: 


src/mongo/db/ops/update.h
----
* 10161 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.h#L365) Invalid modifier specified 
* 12527 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.h#L260) not okForStorage
* 13492 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.h#L285) mod must be RENAME_TO type
* 9015 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.h#L635) 


src/mongo/db/pdfile.cpp
----
* 10003 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1108) failing update: objects in a capped ns cannot grow
* 10083 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L255) create collection invalid size spec
* 10084 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L404) can't map file memory - mongo requires 64 bit build for larger datasets
* 10085 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L406) can't map file memory
* 10086 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L881) ns not found: 
* 10087 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L889) turn off profiling before dropping system.profile collection
* 10089 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1042) can't remove from a capped collection
* 10092 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1410) too may dups on index build with dropDups=true
* 10093 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1925) cannot insert into reserved $ collection
* 10094 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1926) invalid ns: 
* 10095 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1829) attempt to insert in reserved database name 'system'
* 10099 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1964) _id cannot be an array
* 10356 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L347) invalid ns: 
* 10357 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L507) shutdown in progress
* 10358 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L508) bad new extent size
* 10359 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L509) header==0 on new extent: 32 bit mmap space exceeded?
* 10360 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L652) Extent::reset bad magic value
* 10361 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L861) can't create .$freelist
* 12502 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L891) can't drop system ns
* 12503 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L929) 
* 12582 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1769) duplicate key insert for unique index of capped collection
* 12583 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L2079) unexpected index insertion failure on capped collection
* 12584 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1563) cursor gone during bg index
* 12585 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1542) cursor gone during bg index; dropDups
* 12586 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L108) cannot perform operation: a background operation is currently running for this database
* 12587 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L113) cannot perform operation: a background operation is currently running for this collection
* 13130 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1579) can't start bg index b/c in recursive lock (db.eval?)
* 13143 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1870) can't create index on system.indexes" , tabletoidxns.find( ".system.indexes
* 13440 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L387) 
* 13441 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L381) 
* 13596 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1103) cannot change _id of a document old:" << objOld << " new:
* 14037 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L231) can't create user databases on a --configsvr instance
* 14051 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1837) system.users entry needs 'user' field to be a string" , t["user
* 14052 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1838) system.users entry needs 'pwd' field to be a string" , t["pwd
* 14053 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1839) system.users entry needs 'user' field to be non-empty" , t["user
* 14054 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1840) system.users entry needs 'pwd' field to be non-empty" , t["pwd
* 16093 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1534) after yield cursor deleted


src/mongo/db/pdfile.h
----
* 13640 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.h#L382) DataFileHeader looks corrupt at file open filelength:" << filelength << " fileno:


src/mongo/db/pipeline/accumulator.cpp
----
* 15943 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L28) group accumulator 
* 16030 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L59) reserved error
* 16031 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L60) reserved error
* 16032 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L61) reserved error
* 16033 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L62) reserved error
* 16036 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L64) reserved error
* 16037 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L65) reserved error
* 16038 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L66) reserved error
* 16039 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L67) reserved error
* 16040 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L68) reserved error
* 16041 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L69) reserved error
* 16042 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L70) reserved error
* 16043 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L71) reserved error
* 16044 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L72) reserved error
* 16045 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L73) reserved error
* 16046 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L74) reserved error
* 16047 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L75) reserved error
* 16048 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L76) reserved error
* 16049 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L77) reserved error


src/mongo/db/pipeline/doc_mem_monitor.cpp
----
* 15944 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/doc_mem_monitor.cpp#L55) terminating request:  request heap use exceeded 10% of physical RAM


src/mongo/db/pipeline/document.cpp
----
* 15945 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document.cpp#L114) cannot add undefined field 
* 15968 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document.cpp#L132) cannot set undefined field 


src/mongo/db/pipeline/document_source_filter.cpp
----
* 15946 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_filter.cpp#L74) a document filter expression must be an object


src/mongo/db/pipeline/document_source_group.cpp
----
* 15947 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L147) a group's fields must be specified in an object
* 15948 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L161) a group's _id may only be specified once
* 15949 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L219) 
* 15950 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L230) 
* 15951 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L235) 
* 15952 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L254) 
* 15953 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L269) 
* 15954 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L281) 
* 15955 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L288) a group specification must include an _id


src/mongo/db/pipeline/document_source_limit.cpp
----
* 15957 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_limit.cpp#L93) the limit must be specified as a number
* 15958 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_limit.cpp#L100) the limit must be positive


src/mongo/db/pipeline/document_source_match.cpp
----
* 15959 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_match.cpp#L66) the match filter must be an expression in an object


src/mongo/db/pipeline/document_source_project.cpp
----
* 15960 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_project.cpp#L102) 
* 15961 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_project.cpp#L111) 
* 15969 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_project.cpp#L134) 
* 15970 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_project.cpp#L176) 
* 15971 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_project.cpp#L213) 
* 15984 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_project.cpp#L239) 


src/mongo/db/pipeline/document_source_skip.cpp
----
* 15956 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_skip.cpp#L118) 
* 15972 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_skip.cpp#L110) 


src/mongo/db/pipeline/document_source_sort.cpp
----
* 15973 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_sort.cpp#L116)  the 
* 15974 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_sort.cpp#L131) 
* 15975 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_sort.cpp#L136) 
* 15976 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_sort.cpp#L144) 


src/mongo/db/pipeline/document_source_unwind.cpp
----
* 15977 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_unwind.cpp#L108) 
* 15978 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_unwind.cpp#L121) 
* 15979 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_unwind.cpp#L213) 
* 15980 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_unwind.cpp#L217) the path of the field to unwind cannot be empty
* 15981 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_unwind.cpp#L230) the 


src/mongo/db/pipeline/expression.cpp
----
* 15982 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L59) 
* 15983 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L91) 
* 15990 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L102) this object is already an operator expression, and can't be used as a document expression (at \"
* 15991 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L146) 
* 15992 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L164) 
* 15993 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2604) 
* 15994 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L801) 
* 15995 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L843) 
* 15996 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2941) cannot subtract one date from another
* 15997 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2611) 
* 15999 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L241) invalid operator \"
* 16000 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L412) can't add two dates together
* 16008 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1335) 
* 16009 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1341) 
* 16010 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1348) 
* 16011 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1363) 
* 16012 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1376) 
* 16013 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1388) 
* 16014 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1630) 
* 16015 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1644) 
* 16019 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L252) the 
* 16020 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L275) the 
* 16021 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L259) the 
* 16022 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L289) the 
* 16023 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2021) 
* 16024 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2059) 
* 16025 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2099) 
* 16026 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2106) 
* 16027 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2069) 
* 16034 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2884) 
* 16035 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2890) 


src/mongo/db/pipeline/field_path.cpp
----
* 15998 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/field_path.cpp#L53) 


src/mongo/db/pipeline/value.cpp
----
* 16001 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L86) 
* 16002 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L180) 
* 16003 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L543) 
* 16004 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L569) 
* 16005 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L595) 
* 16006 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L615) 
* 16007 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L651) 
* 16016 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L739) 
* 16017 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L790) 
* 16018 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L888) 


src/mongo/db/projection.cpp
----
* 10053 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L82) You cannot currently mix including and excluding fields. Contact us if this is an issue.
* 10371 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L25) can only add to Projection once
* 13097 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L64) Unsupported projection option: 
* 13098 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L60) $slice only supports numbers and [skip, limit] arrays
* 13099 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L50) $slice array wrong size
* 13100 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L55) $slice limit must be positive


src/mongo/db/queryoptimizer.cpp
----
* 10111 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L287) table scans not allowed:
* 10112 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L412) bad hint
* 10113 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L424) bad hint
* 10363 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L228) newCursor() with start location not implemented for indexed plans
* 10364 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L249) newReverseCursor() not implemented for indexed plans
* 10365 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L388) 
* 10366 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L450) natural order cannot be specified with $min/$max
* 10367 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L462) 
* 10368 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L529) Unable to locate previously recorded index
* 10369 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L767) no plans
* 13038 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L503) can't find special index: " + _special + " for: 
* 13040 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L104) no type for special: 
* 13268 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L960) invalid $or spec
* 13292 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L399) hint eoo
* 14820 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L305) doing _id query on a capped collection 


src/mongo/db/queryoptimizer.h
----
* 13266 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.h#L511) not implemented for $or query
* 13271 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.h#L514) can't run more ops


src/mongo/db/queryoptimizercursorimpl.cpp
----
* 14809 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizercursorimpl.cpp#L647) Invalid access for cursor that is not ok()
* 9011 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizercursorimpl.cpp#L80) 


src/mongo/db/queryutil-inl.h
----
* 14049 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil-inl.h#L114) FieldRangeSetPair invalid index specified


src/mongo/db/queryutil.cpp
----
* 10370 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L347) $all requires array
* 12580 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L176) invalid query
* 13033 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L719) can't have 2 special fields
* 13034 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L911) invalid use of $not
* 13041 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L920) invalid use of $not
* 13050 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L829) $all requires array
* 13262 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L1478) $or requires nonempty array
* 13263 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L1482) $or array must contain objects
* 13274 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L1494) no or clause to pop
* 13291 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L1484) $or may not contain 'special' query
* 13303 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L1084) combinatorial limit of $in partitioning of result set exceeded
* 13304 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L1094) combinatorial limit of $in partitioning of result set exceeded
* 13385 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L974) combinatorial limit of $in partitioning of result set exceeded
* 13454 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L252) invalid regular expression operator
* 14048 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L1177) FieldRangeSetPair invalid index specified
* 14816 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L867) $and expression must be a nonempty array
* 14817 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L871) $and elements must be objects
* 15881 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L180) $elemMatch not allowed within $in


src/mongo/db/queryutil.h
----
* 10102 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.h#L53) bad order array
* 10103 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.h#L54) bad order array [2]
* 10104 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.h#L57) too many ordering elements
* 10105 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.h#L141) bad skip value in query
* 12001 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.h#L220) E12001 can't sort with $snapshot
* 12002 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.h#L221) E12002 can't use hint with $snapshot
* 13513 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.h#L190) sort must be an object or array


src/mongo/db/repl.cpp
----
* 10002 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L392) local.sources collection corrupt?
* 10118 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L260) 'host' field not set in sources collection object
* 10119 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L261) only source='main' allowed for now with replication", sourceName() == "main
* 10120 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L264) bad sources 'syncedTo' field value
* 10123 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L1006) replication error last applied optime at slave >= nextOpTime from master
* 10124 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L1238) 
* 10384 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L403) --only requires use of --source
* 10385 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L459) Unable to get database list
* 10386 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L775) non Date ts found: 
* 10389 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L804) Unable to get database list
* 10390 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L894) got $err reading remote oplog
* 10391 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L899) repl: bad object read from remote oplog
* 10392 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L1074) bad user object? [1]
* 10393 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L1075) bad user object? [2]
* 13344 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L890) trying to slave off of a non-master
* 14032 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L564) Invalid 'ts' in remote log
* 14033 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L570) Unable to get database list
* 14034 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L612) Duplicate database names present after attempting to delete duplicates
* 15914 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L623) Failure retrying initial sync update


src/mongo/db/repl/health.h
----
* 13112 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/health.h#L41) bad replset heartbeat option
* 13113 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/health.h#L42) bad replset heartbeat option


src/mongo/db/repl/heartbeat.cpp
----
* 15900 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/heartbeat.cpp#L146) can't heartbeat: too much lock


src/mongo/db/repl/rs.cpp
----
* 13093 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs.cpp#L303) bad --replSet config string format is: <setname>[/<seedhost1>,<seedhost2>,...]
* 13096 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs.cpp#L322) bad --replSet command line config string - dups?
* 13101 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs.cpp#L324) can't use localhost in replset host list
* 13114 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs.cpp#L320) bad --replSet seed hostname
* 13290 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs.cpp#L383) bad replSet oplog entry?
* 13302 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs.cpp#L487) replSet error self appears twice in the repl set configuration


src/mongo/db/repl/rs_config.cpp
----
* 13107 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L533) 
* 13108 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L543) bad replset config -- duplicate hosts in the config object?
* 13109 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L649) multiple rows in " << rsConfigNs << " not supported host: 
* 13115 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L464) bad " + rsConfigNs + " config: version
* 13117 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L550) bad " + rsConfigNs + " config
* 13122 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L567) bad repl set config?
* 13126 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L141) bad Member config
* 13131 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L474) replSet error parsing (or missing) 'members' field in config object
* 13132 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L321) 
* 13133 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L325) replSet bad config no members
* 13135 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L539) 
* 13260 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L624) 
* 13308 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L324) replSet bad config version #
* 13309 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L326) replSet bad config maximum number of members is 12
* 13393 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L549) can't use localhost in repl set member names except when using it for all members
* 13419 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L148) priorities must be between 0.0 and 100.0
* 13432 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L273) _id may not change for members
* 13433 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L290) can't find self in new replset config
* 13434 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L44) unexpected field '" << e.fieldName() << "' in object
* 13437 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L149) slaveDelay requires priority be zero
* 13438 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L150) bad slaveDelay value
* 13439 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L151) priority must be 0 when hidden=true
* 13476 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L277) buildIndexes may not change for members
* 13477 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L152) priority must be 0 when buildIndexes=false
* 13510 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L283) arbiterOnly may not change for members
* 13612 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L333) replSet bad config maximum number of voting members is 7
* 13613 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L334) replSet bad config no voting members
* 13645 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L267) hosts cannot switch between localhost and hostname
* 14046 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L383) getLastErrorMode rules must be objects
* 14827 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L525) arbiters cannot have tags
* 14828 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L395) getLastErrorMode criteria must be greater than 0: 
* 14829 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L390) getLastErrorMode criteria must be numeric
* 14831 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L400) mode " << clauseObj << " requires 


src/mongo/db/repl/rs_initialsync.cpp
----
* 13404 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_initialsync.cpp#L41) 


src/mongo/db/repl/rs_initiate.cpp
----
* 13144 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_initiate.cpp#L130) 
* 13145 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_initiate.cpp#L93) set name does not match the set name host " + i->h.toString() + " expects
* 13256 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_initiate.cpp#L97) member " + i->h.toString() + " is already initiated
* 13259 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_initiate.cpp#L83) 
* 13278 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_initiate.cpp#L58) bad config: isSelf is true for multiple hosts: 
* 13279 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_initiate.cpp#L64) 
* 13311 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_initiate.cpp#L136) member " + i->h.toString() + " has data already, cannot initiate set.  All members except initiator must be empty.
* 13341 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_initiate.cpp#L102) member " + i->h.toString() + " has a config version >= to the new cfg version; cannot change config
* 13420 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_initiate.cpp#L51) initiation and reconfiguration of a replica set must be sent to a node that can become primary


src/mongo/db/repl/rs_rollback.cpp
----
* 13410 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_rollback.cpp#L346) replSet too much data to roll back
* 13423 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_rollback.cpp#L457) replSet error in rollback can't find 
* 15909 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_rollback.cpp#L400) replSet rollback error resyncing collection 


src/mongo/db/repl/rs_sync.cpp
----
* 1000 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_sync.cpp#L357) replSet source for syncing doesn't seem to be await capable -- is it an older version of mongodb?
* 12000 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_sync.cpp#L462) rs slaveDelay differential too big check clocks and systems
* 13508 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_sync.cpp#L100) no 'ts' in first op in oplog: 
* 15915 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_sync.cpp#L220) replSet update still fails after adding missing object
* 16113 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_sync.cpp#L639) 


src/mongo/db/repl_block.cpp
----
* 14830 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl_block.cpp#L147) unrecognized getLastError mode: 


src/mongo/db/replutil.h
----
* 10107 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/replutil.h#L84) not master
* 13435 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/replutil.h#L92) not master and slaveOk=false
* 13436 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/replutil.h#L93) not master or secondary; cannot currently read from this replSet member


src/mongo/db/restapi.cpp
----
* 13085 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/restapi.cpp#L150) query failed for dbwebserver


src/mongo/db/scanandorder.cpp
----
* 15925 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/scanandorder.cpp#L37) cannot sort with keys that are parallel arrays


src/mongo/db/security.cpp
----
* 15889 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/security.cpp#L77) key file must be used to log in with internal user


src/mongo/dbtests/framework.cpp
----
* 10162 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/dbtests/framework.cpp#L405) already have suite with that name


src/mongo/dbtests/jsobjtests.cpp
----
* 12528 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/dbtests/jsobjtests.cpp#L1887) should be ok for storage:
* 12529 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/dbtests/jsobjtests.cpp#L1894) should NOT be ok for storage:


src/mongo/s/balance.cpp
----
* 13258 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/balance.cpp#L309) oids broken after resetting!


src/mongo/s/chunk.cpp
----
* 10163 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L129) can only handle numbers here - which i think is correct
* 10165 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L275) can't split as shard doesn't have a manager
* 10167 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L313) can't move shard to its current location!
* 10169 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L445) datasize failed!" , conn->runCommand( "admin
* 10170 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L67) Chunk needs a ns
* 10171 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L70) Chunk needs a server
* 10172 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L72) Chunk needs a min
* 10173 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L73) Chunk needs a max
* 10174 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L924) config servers not all up
* 10412 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L423) 
* 13003 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L278) can't split a chunk with only one distinct value
* 13141 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L792) Chunk map pointed to incorrect chunk
* 13282 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L604) Couldn't load a valid config for " + _ns + " after 3 attempts. Please try again.
* 13327 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L68) Chunk ns must match server ns
* 13331 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L922) collection's metadata is undergoing changes. Please try again.
* 13332 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L276) need a split key to split chunk
* 13333 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L277) can't split a chunk in that many parts
* 13345 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L200) 
* 13405 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L871) min value " << min << " does not have shard key
* 13406 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L872) max value " << max << " does not have shard key
* 13501 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L817) use geoNear command rather than $near query
* 13502 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L824) unrecognized special query type: 
* 13503 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L171) 
* 13507 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L878) no chunks found between bounds " << min << " and 
* 14022 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L919) Error locking distributed lock for chunk drop.
* 15903 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L752) 
* 16068 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L863) no chunk ranges available
* 8070 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L796) couldn't find a chunk which should be impossible: 
* 8071 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L962) cleaning up after drop failed: 


src/mongo/s/client.cpp
----
* 13134 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/client.cpp#L65) 


src/mongo/s/commands_public.cpp
----
* 10418 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L298) how could chunk manager be null!
* 10420 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L806) how could chunk manager be null!
* 12594 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L550) how could chunk manager be null!
* 13002 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L672) shard internal error chunk manager should never be null
* 13091 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L871) how could chunk manager be null!
* 13092 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L872) GridFS chunks collection can only be sharded on files_id", cm->getShardKey().key() == BSON("files_id
* 13137 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L355) Source and destination collections must be on same shard
* 13138 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L349) You can't rename a sharded collection
* 13139 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L350) You can't rename to a sharded collection
* 13140 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L348) Don't recognize source or target DB
* 13343 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L675) query for sharded findAndModify must have shardkey
* 13398 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L369) cant copy to sharded DB
* 13399 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L377) need a fromdb argument
* 13400 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L380) don't know where source DB is
* 13401 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L381) cant copy from sharded DB
* 13402 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L366) need a todb argument
* 13407 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L708) how could chunk manager be null!
* 13408 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L714) keyPattern must equal shard key
* 13500 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L902) how could chunk manager be null!
* 13512 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L301) drop collection attempted on non-sharded collection
* 15920 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L1099) Cannot output to a non-sharded collection, a sharded collection exists


src/mongo/s/config.cpp
----
* 10176 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L576) shard state missing for 
* 10178 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L130) no primary!
* 10181 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L283) not sharded:
* 10184 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L579) _dropShardedCollections too many collections - bailing
* 10187 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L613) need configdbs
* 10189 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L779) should only have 1 thing in config.version
* 13396 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L446) DBConfig save failed: 
* 13449 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L163) collections already sharded
* 13473 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L93) failed to save collection (" + ns + "): 
* 13509 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L396) can't migrate from 1.5.x release to the current one; need to upgrade to 1.6.x first
* 13648 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L150) can't shard collection because not all config servers are up
* 14822 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L353) state changed in the middle: 
* 15883 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L368) not sharded after chunk manager reset : 
* 15885 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L309) not sharded after reloading from chunks : 
* 8042 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L149) db doesn't have sharding enabled
* 8043 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L157) collection already sharded


src/mongo/s/config.h
----
* 10190 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.h#L217) ConfigServer not setup
* 8041 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.h#L162) no primary shard configured for db: 


src/mongo/s/cursors.cpp
----
* 10191 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/cursors.cpp#L91) cursor already done
* 13286 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/cursors.cpp#L239) sent 0 cursors to kill
* 13287 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/cursors.cpp#L240) too many cursors to kill


src/mongo/s/d_chunk_manager.cpp
----
* 13539 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_chunk_manager.cpp#L49)  does not exist
* 13540 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_chunk_manager.cpp#L50)  collection config entry corrupted" , collectionDoc["dropped
* 13541 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_chunk_manager.cpp#L51)  dropped. Re-shard collection first." , !collectionDoc["dropped
* 13542 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_chunk_manager.cpp#L77) collection doesn't have a key: 
* 13585 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_chunk_manager.cpp#L237) version " << version.toString() << " not greater than 
* 13586 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_chunk_manager.cpp#L206) couldn't find chunk " << min << "->
* 13587 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_chunk_manager.cpp#L214) 
* 13588 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_chunk_manager.cpp#L271) 
* 13590 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_chunk_manager.cpp#L228) setting version to " << version << " on removing last chunk
* 13591 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_chunk_manager.cpp#L257) version can't be set to zero
* 14039 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_chunk_manager.cpp#L296) version " << version.toString() << " not greater than 
* 14040 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_chunk_manager.cpp#L303) can split " << min << " -> " << max << " on 


src/mongo/s/d_logic.cpp
----
* 10422 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_logic.cpp#L100) write with bad shard config and no server id!
* 9517 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_logic.cpp#L93) writeback


src/mongo/s/d_split.cpp
----
* 13593 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_split.cpp#L777) 


src/mongo/s/d_state.cpp
----
* 13298 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_state.cpp#L78) 
* 13299 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_state.cpp#L100) 
* 13647 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_state.cpp#L573) context should be empty here, is: 


src/mongo/s/grid.cpp
----
* 10185 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/grid.cpp#L98) can't find a shard to put new db on
* 10186 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/grid.cpp#L112) removeDB expects db name
* 10421 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/grid.cpp#L479) getoptime failed" , conn->simpleCommand( "admin" , &result , "getoptime
* 15918 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/grid.cpp#L43) invalid database name: 


src/mongo/s/request.cpp
----
* 10194 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/request.cpp#L104) can't call primaryShard on a sharded collection!
* 13644 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/request.cpp#L75) can't use 'local' database through mongos" , ! str::startsWith( getns() , "local.
* 15845 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/request.cpp#L57) 
* 8060 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/request.cpp#L100) can't call primaryShard on a sharded collection


src/mongo/s/security.cpp
----
* 15890 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/security.cpp#L35) key file must be used to log in with internal user


src/mongo/s/server.cpp
----
* 10197 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/server.cpp#L182) createDirectClient not implemented for sharding yet
* 15849 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/server.cpp#L88) client info not defined


src/mongo/s/shard.cpp
----
* 13128 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard.cpp#L123) can't find shard for: 
* 13129 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard.cpp#L105) can't find shard for: 
* 13136 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard.cpp#L327) 
* 13632 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard.cpp#L40) couldn't get updated shard list from config server
* 14807 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard.cpp#L271) no set name for shard: " << _name << " 
* 15847 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard.cpp#L384) can't authenticate to shard server
* 15907 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard.cpp#L396) could not initialize sharding on connection 


src/mongo/s/shard_version.cpp
----
* 10428 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard_version.cpp#L232) need_authoritative set but in authoritative mode already
* 10429 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard_version.cpp#L261) 
* 15904 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard_version.cpp#L79) cannot set version on invalid connection 
* 15905 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard_version.cpp#L84) cannot set version or shard on pair connection 
* 15906 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard_version.cpp#L87) cannot set version or shard on sync connection 


src/mongo/s/shardkey.cpp
----
* 10198 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shardkey.cpp#L46) left object ("  << lObject << ") doesn't have full shard key (
* 10199 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shardkey.cpp#L49) right object (" << rObject << ") doesn't have full shard key (


src/mongo/s/shardkey.h
----
* 13334 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shardkey.h#L127) Shard Key must be less than 512 bytes


src/mongo/s/strategy.cpp
----
* 10200 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy.cpp#L68) mongos: error calling db


src/mongo/s/strategy_shard.cpp
----
* 10201 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L328) invalid update
* 10203 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L440) bad delete message
* 10205 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L539) can't use unique indexes with sharding  ns:
* 12376 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L372) full shard key must be in update object for collection: 
* 13123 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L352) Can't modify shard key's value. field: 
* 13505 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L442) $atomic not supported sharded" , pattern["$atomic
* 13506 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L333) $atomic not supported sharded" , !query.hasField("$atomic
* 14804 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L282) collection no longer sharded
* 14805 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L474) collection no longer sharded
* 14806 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L430) collection no longer sharded
* 16055 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L219) too many retries during bulk insert, " << insertsRemaining.size() << " inserts remaining
* 16056 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L220) shutting down server during bulk insert, " << insertsRemaining.size() << " inserts remaining
* 16064 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L347) can't mix $operator style update with non-$op fields
* 16065 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L370) multi-updates require $ops rather than replacement object
* 16066 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L409) 
* 8010 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L52) something is wrong, shouldn't see a command here
* 8011 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L205) tried to insert object with no valid shard key for " << manager->getShardKey().toString() << " : 
* 8012 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L393) can't upsert something without full valid shard key
* 8013 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L366) For non-multi updates, must have _id or full shard key in query
* 8014 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L380) cannot modify shard key for collection: 
* 8015 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L455) can only delete with a non-shard key pattern if can delete as many as we find
* 8016 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L522) can't do this write op on sharded collection
* 8050 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L560) can't update system.indexes
* 8051 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L564) can't delete indexes on sharded collection yet
* 8052 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L568) handleIndexWrite invalid write op


src/mongo/s/strategy_single.cpp
----
* 10204 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_single.cpp#L107) dbgrid: getmore: error calling db
* 13390 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_single.cpp#L87) unrecognized command: 


src/mongo/s/util.h
----
* 13657 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/util.h#L108) unknown type for ShardChunkVersion: 


src/mongo/s/writeback_listener.cpp
----
* 10427 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/writeback_listener.cpp#L163) invalid writeback message
* 13403 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/writeback_listener.cpp#L111) didn't get writeback for: " << oid << " after: " << t.millis() << " ms
* 13641 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/writeback_listener.cpp#L70) can't parse host [" << conn.getServerAddress() << "]
* 14041 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/writeback_listener.cpp#L101) got writeback waitfor for older id 
* 15884 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/writeback_listener.cpp#L238) Could not reload chunk manager after " << attempts << " attempts.


src/mongo/scripting/bench.cpp
----
* 14811 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/bench.cpp#L143) invalid bench dynamic piece: 
* 15930 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/bench.cpp#L697) Authenticating to connection for bench run failed: 
* 15931 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/bench.cpp#L208) Authenticating to connection for _benchThread failed: 
* 15932 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/bench.cpp#L473) Authenticating to connection for benchThread failed: 


src/mongo/scripting/engine.cpp
----
* 10206 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine.cpp#L85) 
* 10207 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine.cpp#L92) compile failed
* 10208 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine.cpp#L178) need to have locallyConnected already
* 10209 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine.cpp#L199) name has to be a string: 
* 10210 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine.cpp#L200) value has to be set
* 10430 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine.cpp#L170) invalid object id: not hex
* 10448 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine.cpp#L161) invalid object id: length


src/mongo/scripting/engine.h
----
* 13474 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine.h#L206) no _getInterruptSpecCallback
* 9004 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine.h#L93) invoke failed: 
* 9005 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine.h#L102) invoke failed: 


src/mongo/scripting/engine_spidermonkey.cpp
----
* 10212 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L81) holder magic value is wrong
* 10213 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L221) non ascii character detected
* 10214 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L249) not a number
* 10215 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L325) not an object
* 10216 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L334) not a function
* 10217 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L391) can't append field.  name:" + name + " type: 
* 10218 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L728) not done: toval
* 10219 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L755) object passed to getPropery is null
* 10220 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L846) don't know what to do with this op
* 10221 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1173) JS_NewRuntime failed
* 10222 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1181) assert not being executed
* 10223 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1256) deleted SMScope twice?
* 10224 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1319) already local connected
* 10225 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1329) already setup for external db
* 10226 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1331) connected to different db
* 10227 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1400) unknown type
* 10228 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1556)  exec failed: 
* 10229 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1755) need a scope
* 10431 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1232) JS_NewContext failed
* 10432 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1239) JS_NewObject failed for global
* 10433 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1241) js init failed
* 13072 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L34) JS_NewObject failed: 
* 13076 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L151) recursive toObject
* 13498 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L214) 
* 13615 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L38) JS allocation failed, either memory leak or using too much memory
* 9006 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L44) invalid utf8


src/mongo/scripting/engine_v8.cpp
----
* 10230 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L646) can't handle external yet
* 10231 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L691) not an object
* 10232 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L753) not a func
* 10233 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L863) 
* 10234 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L890) 
* 12509 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L654) don't know what this is: 
* 12510 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L955) externalSetup already called, can't call externalSetup
* 12511 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L959) localConnect called with a different name previously
* 12512 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L978) localConnect already called, can't call externalSetup
* 13475 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L873) 


src/mongo/scripting/sm_db.cpp
----
* 10235 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/sm_db.cpp#L79) no cursor!
* 10236 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/sm_db.cpp#L84) no args to internal_cursor_constructor
* 10237 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/sm_db.cpp#L157) mongo_constructor not implemented yet
* 10239 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/sm_db.cpp#L215) no connection!
* 10245 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/sm_db.cpp#L309) no connection!
* 10248 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/sm_db.cpp#L337) no connection!
* 10251 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/sm_db.cpp#L396) no connection!


src/mongo/scripting/utils.cpp
----
* 10261 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/utils.cpp#L29) js md5 needs a string


src/mongo/shell/shell_utils.cpp
----
* 10257 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L128) need to specify 1 argument to listFiles
* 10258 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L109) processinfo not supported
* 12513 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L973) connect failed", scope.exec( _dbConnect , "(connect)
* 12514 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L976) login failed", scope.exec( _dbAuth , "(auth)
* 12518 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L883) srand requires a single numeric argument
* 12519 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L894) rand accepts no arguments
* 12581 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L137) 
* 12597 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L205) need to specify 1 argument
* 13006 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L905) isWindows accepts no arguments
* 13301 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L225) cat() : file to big to load as a variable
* 13411 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L921) getHostName accepts no arguments
* 13619 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L276) fuzzFile takes 2 arguments
* 13620 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L279) couldn't open file to fuzz
* 13621 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L650) no known mongo program on port
* 14042 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L557) 
* 15852 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L848) stopMongoByPid needs a number
* 15853 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L839) stopMongo needs a number


src/mongo/tools/dump.cpp
----
* 10262 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/dump.cpp#L124) couldn't open file
* 14035 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/dump.cpp#L78) couldn't write to file
* 15933 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/dump.cpp#L139) Couldn't open file: 


src/mongo/tools/import.cpp
----
* 10263 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/import.cpp#L124) unknown error reading file
* 13289 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/import.cpp#L134) Invalid UTF8 character detected
* 13293 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/import.cpp#L155) BSON representation of supplied JSON array is too large: 
* 13295 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/import.cpp#L117) JSONArray file too large
* 13504 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/import.cpp#L189) BSON representation of supplied JSON is too large: 
* 15854 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/import.cpp#L216) CSV file ends while inside quoted field


src/mongo/tools/restore.cpp
----
* 15934 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/restore.cpp#L431) JSON object size didn't match file size
* 15935 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/restore.cpp#L85) user does not have write access
* 15936 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/restore.cpp#L495) Creating collection " + _curns + " failed. Errmsg: " + info["errmsg


src/mongo/tools/sniffer.cpp
----
* 10266 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/sniffer.cpp#L482) can't use --source twice
* 10267 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/sniffer.cpp#L483) source needs more args


src/mongo/tools/tool.cpp
----
* 10264 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/tool.cpp#L494) invalid object size: 
* 10265 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/tool.cpp#L530) counts don't match
* 9997 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/tool.cpp#L433) authentication failed: 
* 9998 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/tool.cpp#L396) you need to specify fields
* 9999 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/tool.cpp#L375) file: " + fn ) + " doesn't exist


src/mongo/util/alignedbuilder.cpp
----
* 13524 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/alignedbuilder.cpp#L109) out of memory AlignedBuilder
* 13584 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/alignedbuilder.cpp#L27) out of memory AlignedBuilder


src/mongo/util/assert_util.h
----
* 10437 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L240) unknown exception
* 123 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L75) blah
* 13294 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L238) 
* 14043 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L249) 
* 14044 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L251) unknown exception


src/mongo/util/background.cpp
----
* 13643 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/background.cpp#L52) backgroundjob already started: 


src/mongo/util/base64.cpp
----
* 10270 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/base64.cpp#L79) invalid base64


src/mongo/util/concurrency/list.h
----
* 14050 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/list.h#L84) List1: item to orphan not in list


src/mongo/util/concurrency/qlock.h
----
* 16136 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L221) 
* 16137 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L259) 
* 16138 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L265) 
* 16139 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L271) 
* 16140 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L277) 


src/mongo/util/file.h
----
* 10438 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file.h#L117) ReadFile error - truncated file?


src/mongo/util/file_allocator.cpp
----
* 10439 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L289) 
* 10440 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L191) 
* 10441 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L195) Unable to allocate new file of size 
* 10442 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L197) Unable to allocate new file of size 
* 10443 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L212) FileAllocator: file write failed
* 13653 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L308) 
* 16062 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L157) fstatfs failed: 
* 16063 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L174) ftruncate failed: 


src/mongo/util/log.cpp
----
* 10268 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/log.cpp#L53) LoggingManager already started
* 14036 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/log.cpp#L97) couldn't write to log file


src/mongo/util/logfile.cpp
----
* 13514 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L242) error appending to file on fsync 
* 13515 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L232) error appending to file 
* 13516 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L174) couldn't open file " << name << " for writing 
* 13517 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L126) error appending to file 
* 13518 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L70) couldn't open file " << name << " for writing 
* 13519 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L124) error 87 appending to file - invalid parameter
* 15871 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L84) Couldn't truncate file: 
* 15873 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L192) Couldn't truncate file: 


src/mongo/util/mmap.cpp
----
* 13468 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap.cpp#L40) can't create file already exists 
* 13617 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap.cpp#L192) MongoFile : multiple opens of same filename
* 15922 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap.cpp#L64) couldn't get file length when opening mapping 
* 15923 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap.cpp#L74) couldn't get file length when opening mapping 


src/mongo/util/mmap_posix.cpp
----
* 10446 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap_posix.cpp#L98) mmap: can't map area of size 0 file: 
* 10447 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap_posix.cpp#L108) map file alloc failed, wanted: " << length << " filelen: 


src/mongo/util/mmap_win.cpp
----
* 13056 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap_win.cpp#L217) Async flushing not supported on windows


src/mongo/util/net/hostandport.h
----
* 13095 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/hostandport.h#L216) HostAndPort: bad port #
* 13110 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/hostandport.h#L210) HostAndPort: host is empty


src/mongo/util/net/httpclient.cpp
----
* 10271 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/httpclient.cpp#L47) invalid url" , url.find( "http://
* 15862 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/httpclient.cpp#L108) no ssl support


src/mongo/util/net/listen.cpp
----
* 15863 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/listen.cpp#L133) listen(): invalid socket? 


src/mongo/util/net/message.h
----
* 13273 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/message.h#L170) single data buffer expected


src/mongo/util/net/message_port.cpp
----
* 15901 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/message_port.cpp#L245) client disconnected during operation


src/mongo/util/net/message_server_asio.cpp
----
* 10273 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/message_server_asio.cpp#L110) _cur not empty! pipelining requests not supported
* 10274 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/message_server_asio.cpp#L171) pipelining requests doesn't work yet


src/mongo/util/net/message_server_port.cpp
----
* 10275 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/message_server_port.cpp#L116) multiple PortMessageServer not supported


src/mongo/util/net/sock.cpp
----
* 13079 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L165) path to unix socket too long
* 13080 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L163) no unix socket support on windows
* 13082 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L251) getnameinfo error 
* 15861 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L496) can't create SSL
* 15864 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L454) can't create SSL Context: 
* 15865 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L462) 
* 15866 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L468) 
* 15867 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L485) Can't read certificate file
* 15868 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L490) Can't read key file


src/mongo/util/paths.h
----
* 13600 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/paths.h#L59) 
* 13646 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/paths.h#L88) stat() failed for file: " << path << " 
* 13650 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/paths.h#L116) Couldn't open directory '" << dir.string() << "' for flushing: 
* 13651 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/paths.h#L120) Couldn't fsync directory '" << dir.string() << "': 
* 13652 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/paths.h#L104) Couldn't find parent dir for file: 


src/mongo/util/processinfo_linux2.cpp
----
* 13538 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/processinfo_linux2.cpp#L48) 


src/mongo/util/text.cpp
----
* 16091 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/text.cpp#L95) 


src/mongo/util/text.h
----
* 13305 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/text.h#L131) could not convert string to long long
* 13306 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/text.h#L140) could not convert string to long long
* 13307 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/text.h#L126) cannot convert empty string to long long
* 13310 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/text.h#L144) could not convert string to long long

