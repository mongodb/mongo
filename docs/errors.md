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
* 10335 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonobjbuilder.h#L552) builder does not own memory
* 10336 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonobjbuilder.h#L627) No subobject started
* 13048 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonobjbuilder.h#L778) can't append to array using string field name [" + name.data() + "]
* 15891 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonobjbuilder.h#L786) can't backfill array to larger than 1,500,000 elements


src/mongo/bson/ordering.h
----
* 13103 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/ordering.h#L64) too many compound keys


src/mongo/bson/util/builder.h
----
* 10000 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/util/builder.h#L97) out of memory BufBuilder
* 13548 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/util/builder.h#L209) 
* 15912 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/util/builder.h#L72) out of memory StackAllocator::Realloc
* 15913 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/util/builder.h#L122) out of memory BufBuilder::reset


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
* 10005 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L513) listdatabases failed" , runCommand( "admin" , BSON( "listDatabases
* 10006 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L514) listDatabases.databases not array" , info["databases
* 10007 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L843) dropIndex failed
* 10008 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L850) dropIndexes failed
* 10276 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L575) DBClientBase::findN: transport error: " << getServerAddress() << " ns: " << ns << " query: 
* 10278 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L984) dbclient error communicating with server: 
* 10337 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L936) object not valid
* 11010 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L288) count fails:
* 13386 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L720) socket error for mapping query
* 13421 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L102) trying to connect to invalid ConnectionString


src/mongo/client/dbclient.h
----
* 10011 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.h#L572) no collection name
* 9000 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.h#L865) 


src/mongo/client/dbclient_rs.cpp
----
* 10009 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L236) ReplicaSetMonitor no master found for set: 
* 13610 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L181) ConfigChangeHook already specified
* 13639 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L668) can't connect to new replica set master [" << _masterHost.toString() << "] err: 
* 13642 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L90) need at least 1 node for a replica set
* 15899 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L288) No suitable member found for slaveOk query in replica set: 


src/mongo/client/dbclientcursor.cpp
----
* 13127 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.cpp#L166) getMore: cursor didn't exist on server, possible restart or timeout?
* 13422 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.cpp#L221) DBClientCursor next() called but more() is false
* 14821 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.cpp#L287) No client or lazy client specified, cannot store multi-host connection.
* 15875 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.cpp#L78) 


src/mongo/client/dbclientcursor.h
----
* 13106 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.h#L77) 
* 13348 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.h#L220) connection died
* 13383 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.h#L237) BatchIterator empty


src/mongo/client/distlock.cpp
----
* 14023 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/distlock.cpp#L599) remote time in cluster " << _conn.toString() << " is now skewed, cannot force lock.
* 16060 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/distlock.cpp#L117) cannot query locks collection on config server 


src/mongo/client/distlock_test.cpp
----
* 13678 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/distlock_test.cpp#L382) Could not communicate with server " << server.toString() << " in cluster " << cluster.toString() << " to change skew by 


src/mongo/client/gridfs.cpp
----
* 10012 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L93) file doesn't exist" , fileName == "-
* 10013 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L100) error opening file
* 10014 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L213) chunk is empty!
* 10015 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L245) doesn't exists
* 13296 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L67) invalid chunk size is specified
* 13325 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L239) couldn't open file: 
* 9008 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L139) filemd5 failed


src/mongo/client/model.cpp
----
* 10016 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/model.cpp#L39) _id isn't set - needed for remove()" , _id["_id
* 13121 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/model.cpp#L81) 
* 9002 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/model.cpp#L51) error on Model::remove: 
* 9003 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/model.cpp#L123) error on Model::save: 


src/mongo/client/parallel.cpp
----
* 10017 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L99) cursor already done
* 10018 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L410) no more items
* 10019 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L1501) no more elements
* 13431 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L497) have to have sort key in projection and removing it
* 13633 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L128) error querying server: 
* 14812 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L1572) Error running command on server: 
* 14813 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L1573) Command returned nothing
* 15919 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L712) too many retries for chunk manager or primary
* 15986 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L745) too many retries in total
* 15987 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L866) could not fully initialize cursor on shard " << shard.toString() << ", current connection state is 
* 15988 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L998) error querying server
* 15989 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L700) database not found for parallel cursor request


src/mongo/client/syncclusterconnection.cpp
----
* 10022 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L252) SyncClusterConnection::getMore not supported yet
* 10023 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L274) SyncClusterConnection bulk insert not implemented
* 13053 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L391) help failed: " << info , _commandOnActive( "admin" , BSON( name << "1" << "help
* 13054 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L215) write $cmd not supported in SyncClusterConnection::query for:
* 13104 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L170) SyncClusterConnection::findOne prepare failed: 
* 13105 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L188) 
* 13119 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L259) SyncClusterConnection::insert obj has to have an _id: 
* 13120 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L292) SyncClusterConnection::update upsert query needs _id" , query.obj["_id
* 13397 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L367) SyncClusterConnection::say prepare failed: 
* 15848 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L200) sync cluster of sync clusters?
* 8001 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L135) SyncClusterConnection write op failed: 
* 8002 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L248) all servers down!
* 8003 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L264) SyncClusterConnection::insert prepare failed: 
* 8004 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L50) SyncClusterConnection needs 3 servers
* 8005 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L298) SyncClusterConnection::udpate prepare failed: 
* 8006 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L341) SyncClusterConnection::call can only be used directly for dbQuery
* 8007 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L345) SyncClusterConnection::call can't handle $cmd" , strstr( d.getns(), "$cmd
* 8008 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L361) all servers down!
* 8020 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L280) SyncClusterConnection::remove prepare failed: 


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
* 13000 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/btree.h#L357) invalid keyNode: " +  BSON( "i" << i << "n


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
* 10057 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/client.cpp#L301) 
* 14031 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/client.cpp#L281) Can't take a write lock while out of disk space
* 15928 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/client.cpp#L234) can't open a database from a nested read lock 
* 15929 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/client.cpp#L326) client access to index backing namespace prohibited


src/mongo/db/client.h
----
* 12600 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/client.h#L281) releaseAndWriteLock: unlock_shared failed, probably recursive


src/mongo/db/clientcursor.h
----
* 12051 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/clientcursor.h#L110) clientcursor already in use? driver problem?
* 12521 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/clientcursor.h#L319) internal error: use of an unlocked ClientCursor


src/mongo/db/cloner.cpp
----
* 10024 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L93) bad ns field for index during dbcopy
* 10025 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L95) bad ns field for index during dbcopy [2]
* 10026 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L670) source namespace does not exist
* 10027 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L680) target namespace exists", cmdObj["dropTarget
* 10289 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L299) useReplAuth is not written to replication log
* 10290 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L375) 
* 13008 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L622) must call copydbgetnonce first
* 15908 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L253) 
* 15967 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L659) invalid collection name: 


src/mongo/db/cmdline.cpp
----
* 10033 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cmdline.cpp#L386) logpath has to be non-zero


src/mongo/db/commands.cpp
----
* 15962 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands.cpp#L37) 
* 15966 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands.cpp#L36) admin


src/mongo/db/commands/distinct.cpp
----
* 10044 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/distinct.cpp#L117) distinct too big, 16mb cap


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
* 10076 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L484) rename failed: 
* 10077 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L950) fast_emit takes 2 args
* 13069 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L951) an emit can't be more than half max bson size
* 13070 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L176) value too large to reduce
* 13522 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L261) unknown out specifier [" << t << "]
* 13598 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L55) couldn't compile code for: 
* 13602 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L233) outType is no longer a valid option" , cmdObj["outType
* 13604 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L431) too much data for in memory map/reduce
* 13606 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L275) 'out' has to be a string or an object
* 13608 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L315) query has to be blank or an Object
* 13609 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L322) sort has to be blank or an Object
* 13630 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L361) userCreateNS failed for mr tempLong ns: " << _config.tempLong << " err: 
* 13631 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L346) userCreateNS failed for mr incLong ns: " << _config.incLong << " err: 
* 15876 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L1029) could not create cursor over " << config.ns << " to hold data while prepping m/r
* 15877 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L1031) could not create m/r holding client cursor over 
* 15895 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L271) nonAtomic option cannot be used with this output type
* 15921 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L415) splitVector failed: 
* 16052 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L1080) could not create cursor over " << config.ns << " for query : " << config.filter << " sort : 
* 16053 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L1082) could not create client cursor over " << config.ns << " for query : " << config.filter << " sort : 
* 16054 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L280) shardedFirstPass should only use replace outType
* 9014 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L73) map invoke failed: 


src/mongo/db/commands/pipeline.cpp
----
* 15942 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/pipeline.cpp#L145) pipeline element 


src/mongo/db/compact.cpp
----
* 13660 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/compact.cpp#L282) namespace " << ns << " does not exist
* 13661 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/compact.cpp#L283) cannot compact capped collection
* 14024 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/compact.cpp#L111) compact error out of space during compaction
* 14025 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/compact.cpp#L222) compact error no space available to allocate
* 14027 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/compact.cpp#L274) can't compact a system namespace", !str::contains(ns, ".system.
* 14028 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/compact.cpp#L273) bad ns


src/mongo/db/curop.h
----
* 11600 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/curop.h#L274) interrupted at shutdown
* 11601 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/curop.h#L276) interrupted
* 12601 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/curop.h#L203) CurOp not marked done yet


src/mongo/db/cursor.h
----
* 13285 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cursor.h#L152) manual matcher config not allowed


src/mongo/db/d_concurrency.cpp
----
* 15937 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L192) can't nest lock of " << coll << " beneath 
* 15938 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L195) want collection write lock but it is already read locked 
* 15939 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L218) 
* 15963 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L95) bad collection name: 
* 15964 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L164) bad collection name to lock: 
* 15965 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L199) 


src/mongo/db/database.cpp
----
* 10028 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/database.cpp#L61) db name is empty
* 10029 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/database.cpp#L63) bad db name [1]
* 10030 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/database.cpp#L64) bad db name [2]
* 10031 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/database.cpp#L65) bad char(s) in db name
* 10032 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/database.cpp#L62) db name too long
* 10295 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/database.cpp#L225) getFile(): bad file number value (corrupt db?): run repair
* 12501 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/database.cpp#L304) quota exceeded
* 14810 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/database.cpp#L317) couldn't allocate space (suitableFile)
* 15924 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/database.cpp#L159) getFile(): bad file number value " << n << " (corrupt db?): run repair
* 15927 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/database.cpp#L417) can't open database in a read lock. if db was just closed, consider retrying the query. might otherwise indicate an internal error


src/mongo/db/databaseholder.h
----
* 13074 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/databaseholder.h#L106) db name can't be empty
* 13075 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/databaseholder.h#L109) db name can't be empty
* 13280 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/databaseholder.h#L100) invalid db name: 


src/mongo/db/db.cpp
----
* 10296 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/db.cpp#L463) 
* 10297 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/db.cpp#L1307) Couldn't register Windows Ctrl-C handler
* 12590 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/db.cpp#L468) 
* 14026 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/db.cpp#L286) 


src/mongo/db/db.h
----
* 10298 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/db.h#L40) can't temprelease nested write lock
* 10299 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/db.h#L45) can't temprelease nested read lock
* 14814 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/db.h#L50) 
* 14845 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/db.h#L81) 


src/mongo/db/dbcommands.cpp
----
* 10039 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L800) can't drop collection with reserved $ character in name
* 10040 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L1125) chunks out of order
* 10301 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L1463) source collection " + fromNs + " does not exist
* 13049 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L1598) godinsert must specify a collection
* 13281 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L1144) File deleted during filemd5 command
* 13416 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L1734) captrunc must specify a collection
* 13417 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L1742) captrunc collection not found or empty
* 13418 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L1744) captrunc invalid n
* 13428 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L1761) emptycapped must specify a collection
* 13429 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L1764) emptycapped no such collection
* 14832 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L863) specify size:<n> when capped is true", !cmdObj["capped"].trueValue() || cmdObj["size"].isNumber() || cmdObj.hasField("$nExtents
* 15880 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L604) 
* 15888 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L860) must pass name of collection to create


src/mongo/db/dbcommands_admin.cpp
----
* 12032 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands_admin.cpp#L489) fsync: sync option must be true when using lock
* 12033 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands_admin.cpp#L495) fsync: profiling must be off to enter locked mode
* 12034 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands_admin.cpp#L488) fsync: can't lock while an unlock is pending


src/mongo/db/dbcommands_generic.cpp
----
* 10038 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands_generic.cpp#L371) forced error


src/mongo/db/dbeval.cpp
----
* 10046 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbeval.cpp#L42) eval needs Code
* 12598 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbeval.cpp#L122) $eval reads unauthorized


src/mongo/db/dbhelpers.cpp
----
* 10303 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbhelpers.cpp#L287) {autoIndexId:false}
* 13430 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbhelpers.cpp#L122) no _id index


src/mongo/db/dbmessage.h
----
* 10304 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbmessage.h#L200) Client Error: Remaining data too small for BSON object
* 10305 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbmessage.h#L202) Client Error: Invalid object size
* 10306 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbmessage.h#L203) Client Error: Next object larger than space left in message
* 10307 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbmessage.h#L206) Client Error: bad object in message
* 13066 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbmessage.h#L198) Message contains no documents


src/mongo/db/dbwebserver.cpp
----
* 13453 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbwebserver.cpp#L172) server not started with --jsonp


src/mongo/db/dur.cpp
----
* 13599 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur.cpp#L400) Written data does not match in-memory view. Missing WriteIntent?
* 13616 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur.cpp#L228) can't disable durability with pending writes


src/mongo/db/dur_journal.cpp
----
* 13611 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_journal.cpp#L539) can't read lsn file in journal directory : 
* 13614 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_journal.cpp#L506) unexpected version number of lsn file in journal/ directory got: 
* 15926 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_journal.cpp#L354) Insufficient free space for journals


src/mongo/db/dur_recover.cpp
----
* 13531 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L80) unexpected files in journal directory " << dir.string() << " : 
* 13532 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L87) 
* 13533 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L163) problem processing journal file during recovery
* 13535 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L468) recover abrupt journal file end
* 13536 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L395) journal version number mismatch 
* 13537 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L386) 
* 13544 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L450) recover error couldn't open 
* 13545 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L475) --durOptions " << (int) CmdLine::DurScanOnly << " (scan only) specified
* 13594 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L359) journal checksum doesn't match
* 13622 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L256) Trying to write past end of file in WRITETODATAFILES
* 15874 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dur_recover.cpp#L115) couldn't uncompress journal section


src/mongo/db/durop.cpp
----
* 13546 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/durop.cpp#L53) journal recover: unrecognized opcode in journal 
* 13547 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/durop.cpp#L144) recover couldn't create file 
* 13628 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/durop.cpp#L158) recover failure writing file 


src/mongo/db/extsort.cpp
----
* 10048 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/extsort.cpp#L74) already sorted
* 10049 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/extsort.cpp#L99) sorted already
* 10050 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/extsort.cpp#L120) bad
* 10308 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/extsort.cpp#L231) mmap failed


src/mongo/db/extsort.h
----
* 10052 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/extsort.h#L115) not sorted


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
* 10097 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.cpp#L318) bad table to index name on add index attempt
* 10098 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.cpp#L325) 
* 11001 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.cpp#L98) 
* 12504 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.cpp#L332) 
* 12505 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.cpp#L362) 
* 12523 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.cpp#L313) no index name specified
* 12524 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.cpp#L322) index key pattern too large
* 12588 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.cpp#L368) cannot add index with a background operation in progress
* 14803 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.cpp#L403) this version of mongod cannot build new indexes of version number 
* 14819 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.cpp#L240) 


src/mongo/db/index.h
----
* 14802 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index.h#L166) index v field should be Integer type


src/mongo/db/indexkey.cpp
----
* 13007 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/indexkey.cpp#L65) can only have 1 index plugin / bad index key pattern
* 13529 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/indexkey.cpp#L82) sparse only works for single field keys
* 15855 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/indexkey.cpp#L299) Ambiguous field name found in array (do not use numeric field names in embedded elements in an array), field: '" << arrField.fieldName() << "' for array: 
* 15869 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/indexkey.cpp#L415) Invalid index version for key generation.


src/mongo/db/instance.cpp
----
* 10054 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L530) not master
* 10055 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L512) update object too large
* 10056 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L564) not master
* 10058 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L729) not master
* 10059 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L675) object to insert too large
* 10309 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1048) Unable to create/open lock file: " << name << ' ' << errnoWithDescription() << " Is a mongod instance already running?
* 10310 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1053) Unable to lock file: " + name + ". Is a mongod instance already running?
* 10332 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L92) Expected CurrentTime type
* 12596 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1105) old lock file
* 13004 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L457) sent negative cursors to kill: 
* 13073 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L637) shutting down
* 13342 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1123) Unable to truncate lock file
* 13455 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L950) dbexit timed out getting lock
* 13511 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L681) document to insert can't have $ fields
* 13597 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1115) can't start without --journal enabled when journal/ files are present
* 13618 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1140) can't start without --journal enabled when journal/ files are present
* 13625 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1119) Unable to truncate lock file
* 13627 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1042) Unable to create/open lock file: " << name << ' ' << m << ". Is a mongod instance already running?
* 13637 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L829) count failed in DBDirectClient: 
* 13658 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L456) bad kill cursors size: 
* 13659 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L455) sent 0 cursors to kill


src/mongo/db/jsobj.cpp
----
* 10060 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/jsobj.cpp#L536) woSortOrder needs a non-empty sortKey
* 10061 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/jsobj.cpp#L1133) type not supported for appendMinElementForType
* 10311 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/jsobj.cpp#L95) 
* 10312 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/jsobj.cpp#L253) 
* 12579 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/jsobj.cpp#L855) unhandled cases in BSONObj okForStorage
* 14853 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/jsobj.cpp#L1186) type not supported for appendMaxElementForType


src/mongo/db/json.cpp
----
* 10338 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/json.cpp#L233) Invalid use of reserved field name: 
* 10339 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/json.cpp#L406) Badly formatted bindata
* 10340 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/json.cpp#L642) Failure parsing JSON string near: 


src/mongo/db/lasterror.cpp
----
* 13649 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/lasterror.cpp#L95) no operation yet


src/mongo/db/matcher.cpp
----
* 10066 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L342) $where may only appear once in query
* 10067 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L343) $where query, but no script engine
* 10068 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L206) invalid operator: 
* 10069 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L292) BUG - can't operator for: 
* 10070 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L938) $where compile error
* 10071 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L953) 
* 10072 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L957) unknown error in invocation of $where function
* 10073 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L106) mod can't be 0
* 10341 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L88) scope has to be created first!
* 10342 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L1118) pcre not compiled with utf8 support
* 12517 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L113) $elemMatch needs an Object
* 13020 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L165) with $all, can't mix $elemMatch and others
* 13029 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L283) can't use $not with $options, use BSON regex type instead
* 13030 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L404) $not cannot be empty
* 13031 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L414) invalid use of $not
* 13032 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L272) can't use $not with $regex, use BSON regex type instead
* 13086 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L298) $and/$or/$nor must be a nonempty array
* 13087 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L302) $and/$or/$nor match element must be an object
* 13089 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L344) no current client needed for $where
* 13276 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L231) $in needs an array
* 13277 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L242) $nin needs an array
* 13629 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L363) can't have undefined in a query expression
* 14844 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L441) $atomic specifier must be a top level field
* 15882 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L157) $elemMatch not allowed within $in
* 15892 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L171) 
* 15893 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L176) 
* 15902 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher.cpp#L341) $where expression has an unexpected type


src/mongo/db/mongommf.cpp
----
* 13520 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/mongommf.cpp#L266) MongoMMF only supports filenames in a certain format 
* 13636 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/mongommf.cpp#L295) file " << filename() << " open/create failed in createPrivateMap (look in log for more information)


src/mongo/db/mongomutex.h
----
* 10293 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/mongomutex.h#L276) internal error: locks are not upgradeable: 
* 12599 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/mongomutex.h#L139) internal error: attempt to unlock when wasn't in a write lock
* 13142 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/mongomutex.h#L332) timeout getting readlock


src/mongo/db/namespace-inl.h
----
* 10080 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace-inl.h#L35) ns name too long, max size is 128
* 10348 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace-inl.h#L45) $extra: ns name too long
* 10349 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace-inl.h#L103) E12000 idxNo fails
* 13283 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace-inl.h#L81) Missing Extra
* 14045 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace-inl.h#L82) missing Extra
* 14823 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace-inl.h#L89) missing extra
* 14824 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace-inl.h#L90) missing Extra


src/mongo/db/namespace.cpp
----
* 10079 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace.cpp#L175) bad .ns file length, cannot open database
* 10081 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace.cpp#L486) too many namespaces/collections
* 10082 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace.cpp#L501) allocExtra: too many namespaces/collections
* 10343 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace.cpp#L182) bad lenForNewNsFiles
* 10346 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace.cpp#L564) not implemented
* 10350 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace.cpp#L496) allocExtra: base ns missing?
* 10351 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace.cpp#L497) allocExtra: extra already exists


src/mongo/db/nonce.cpp
----
* 10352 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/nonce.cpp#L33) Security is a singleton class
* 10353 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/nonce.cpp#L43) can't open dev/urandom
* 10354 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/nonce.cpp#L52) md5 unit test fails
* 10355 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/nonce.cpp#L61) devrandom failed


src/mongo/db/oplog.cpp
----
* 13044 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplog.cpp#L516) no ts field in query
* 13257 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplog.cpp#L341) 
* 13288 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplog.cpp#L51) replSet error write op to db before replSet initialized", str::startsWith(ns, "local.
* 13312 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplog.cpp#L138) replSet error : logOp() but not primary?
* 13347 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplog.cpp#L174) local.oplog.rs missing. did you drop it? if so restart server
* 13389 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplog.cpp#L70) local.oplog.rs missing. did you drop it? if so restart server
* 14038 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplog.cpp#L460) invalid _findingStartMode
* 14825 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplog.cpp#L800) error in applyOperation : unknown opType 
* 15916 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplog.cpp#L638) Can no longer connect to initial sync source: 
* 15917 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplog.cpp#L672) Got bad disk location when attempting to insert


src/mongo/db/oplog.h
----
* 14835 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplog.h#L82) 


src/mongo/db/oplogreader.h
----
* 15910 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplogreader.h#L94) Doesn't have cursor for reading oplog
* 15911 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/oplogreader.h#L99) Doesn't have cursor for reading oplog


src/mongo/db/ops/delete.cpp
----
* 10100 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/delete.cpp#L42) cannot delete from collection with reserved $ in name
* 10101 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/delete.cpp#L50) can't remove from a capped collection
* 12050 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/delete.cpp#L38) cannot delete from system namespace


src/mongo/db/ops/query.cpp
----
* 10110 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/query.cpp#L711) bad query object
* 13051 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/query.cpp#L721) tailable cursor requested on non capped collection
* 13052 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/query.cpp#L727) only {$natural:1} order allowed for tailable cursor
* 13530 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/query.cpp#L689) bad or malformed command request?
* 13638 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/query.cpp#L525) client cursor dropped during explain query yield
* 14833 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/query.cpp#L116) auth error


src/mongo/db/ops/query.h
----
* 10102 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/query.h#L52) bad order array
* 10103 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/query.h#L53) bad order array [2]
* 10104 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/query.h#L56) too many ordering elements
* 10105 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/query.h#L135) bad skip value in query
* 12001 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/query.h#L214) E12001 can't sort with $snapshot
* 12002 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/query.h#L215) E12002 can't use hint with $snapshot
* 13513 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/query.h#L184) sort must be an object or array


src/mongo/db/ops/update.cpp
----
* 10131 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L122) $push can only be applied to an array
* 10132 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L193) $pushAll can only be applied to an array
* 10133 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L194) $pushAll has to be passed an array
* 10134 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L218) $pull/$pullAll can only be applied to an array
* 10135 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L251) $pop can only be applied to an array
* 10136 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L287) $bit needs an array
* 10137 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L288) $bit can only be applied to numbers
* 10138 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L289) $bit cannot update a value of type double
* 10139 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L297) $bit field must be number
* 10140 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L407) Cannot apply $inc modifier to non-number
* 10141 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L429) Cannot apply $push/$pushAll modifier to non-array
* 10142 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L435) Cannot apply $pull/$pullAll modifier to non-array
* 10143 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L456) Cannot apply $pop modifier to non-array
* 10145 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L668) LEFT_SUBFIELD only supports Object: " << field << " not: 
* 10147 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L806) Invalid modifier specified: 
* 10148 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L819) Mod on _id not allowed", strcmp( fieldName, "_id
* 10149 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L820) Invalid mod field name, may not end in a period
* 10150 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L821) Field name duplication not allowed with modifiers
* 10151 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L822) have conflicting mods in update
* 10152 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L823) Modifier $inc allowed for numbers only
* 10153 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L824) Modifier $pushAll/pullAll allowed for arrays only
* 10154 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L900) Modifiers and non-modifiers cannot be mixed
* 10155 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L1277) cannot update reserved $ collection
* 10156 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L1280) cannot update system collection: " << ns << " q: " << patternOrig << " u: 
* 10157 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L1125) multi-update requires all modified objects to have an _id
* 10158 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L1235) multi update only works with $ operators
* 10159 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L1263) multi update only works with $ operators
* 10399 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L713) ModSet::createNewFromMods - RIGHT_SUBFIELD should be impossible
* 10400 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L716) unhandled case
* 12522 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L905) $ operator made object too large
* 12591 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L462) Cannot apply $addToSet modifier to non-array
* 12592 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L139) $addToSet can only be applied to an array
* 13478 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L602) can't apply mod in place - shouldn't have gotten here
* 13479 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L831) invalid mod field name, target may not be empty
* 13480 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L832) invalid mod field name, source may not begin or end in period
* 13481 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L833) invalid mod field name, target may not begin or end in period
* 13482 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L834) $rename affecting _id not allowed
* 13483 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L835) $rename affecting _id not allowed
* 13484 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L836) field name duplication not allowed with $rename target
* 13485 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L837) conflicting mods not allowed with $rename target
* 13486 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L838) $rename target may not be a parent of source
* 13487 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L839) $rename source may not be dynamic array", strstr( fieldName , ".$
* 13488 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L840) $rename target may not be dynamic array", strstr( target , ".$
* 13489 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L379) $rename source field invalid
* 13490 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L390) $rename target field invalid
* 13494 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L827) $rename target must be a string
* 13495 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L829) $rename source must differ from target
* 13496 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L830) invalid mod field name, source may not be empty
* 15896 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L818) Modified field name may not start with $
* 9016 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L313) unknown $bit operation: 
* 9017 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L338) 


src/mongo/db/ops/update.h
----
* 10161 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.h#L376) Invalid modifier specified 
* 12527 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.h#L242) not okForStorage
* 13492 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.h#L267) mod must be RENAME_TO type
* 9015 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.h#L640) 


src/mongo/db/pdfile.cpp
----
* 10003 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1102) failing update: objects in a capped ns cannot grow
* 10083 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L249) create collection invalid size spec
* 10084 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L398) can't map file memory - mongo requires 64 bit build for larger datasets
* 10085 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L400) can't map file memory
* 10086 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L875) ns not found: 
* 10087 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L883) turn off profiling before dropping system.profile collection
* 10089 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1036) can't remove from a capped collection
* 10092 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1401) too may dups on index build with dropDups=true
* 10093 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1911) cannot insert into reserved $ collection
* 10094 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1912) invalid ns: 
* 10095 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1815) attempt to insert in reserved database name 'system'
* 10099 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1950) _id cannot be an array
* 10356 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L341) invalid ns: 
* 10357 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L501) shutdown in progress
* 10358 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L502) bad new extent size
* 10359 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L503) header==0 on new extent: 32 bit mmap space exceeded?
* 10360 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L646) Extent::reset bad magic value
* 10361 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L855) can't create .$freelist
* 12502 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L885) can't drop system ns
* 12503 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L923) 
* 12582 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1755) duplicate key insert for unique index of capped collection
* 12583 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L2065) unexpected index insertion failure on capped collection
* 12584 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1551) cursor gone during bg index
* 12585 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1531) cursor gone during bg index; dropDups
* 12586 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L106) cannot perform operation: a background operation is currently running for this database
* 12587 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L111) cannot perform operation: a background operation is currently running for this collection
* 13130 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1565) can't start bg index b/c in recursive lock (db.eval?)
* 13143 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1856) can't create index on system.indexes" , tabletoidxns.find( ".system.indexes
* 13440 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L381) 
* 13441 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L375) 
* 13596 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1097) cannot change _id of a document old:" << objOld << " new:
* 14037 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L225) can't create user databases on a --configsvr instance
* 14051 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1823) system.user entry needs 'user' field to be a string" , t["user
* 14052 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1824) system.user entry needs 'pwd' field to be a string" , t["pwd
* 14053 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1825) system.user entry needs 'user' field to be non-empty" , t["user
* 14054 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.cpp#L1826) system.user entry needs 'pwd' field to be non-empty" , t["pwd


src/mongo/db/pdfile.h
----
* 13640 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pdfile.h#L380) DataFileHeader looks corrupt at file open filelength:" << filelength << " fileno:


src/mongo/db/pipeline/accumulator.cpp
----
* 15943 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L28) group accumulator 
* 15984 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L59) reserved error
* 16023 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L61) reserved error
* 16024 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L62) reserved error
* 16025 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L63) reserved error
* 16026 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L64) reserved error
* 16027 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L65) reserved error
* 16028 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L66) reserved error
* 16029 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L67) reserved error
* 16030 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L68) reserved error
* 16031 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L69) reserved error
* 16032 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L70) reserved error
* 16033 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L71) reserved error
* 16036 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L73) reserved error
* 16037 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L74) reserved error
* 16038 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L75) reserved error
* 16039 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L76) reserved error
* 16040 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L77) reserved error
* 16041 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L78) reserved error
* 16042 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L79) reserved error
* 16043 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L80) reserved error
* 16044 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L81) reserved error
* 16045 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L82) reserved error
* 16046 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L83) reserved error
* 16047 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L84) reserved error
* 16048 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L85) reserved error
* 16049 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator.cpp#L86) reserved error


src/mongo/db/pipeline/doc_mem_monitor.cpp
----
* 15944 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/doc_mem_monitor.cpp#L55) terminating request:  request heap use exceeded 10% of physical RAM


src/mongo/db/pipeline/document.cpp
----
* 15945 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document.cpp#L109) cannot add undefined field 
* 15968 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document.cpp#L127) cannot set undefined field 


src/mongo/db/pipeline/document_source_filter.cpp
----
* 15946 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_filter.cpp#L70) a document filter expression must be an object


src/mongo/db/pipeline/document_source_group.cpp
----
* 15947 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L141) a group's fields must be specified in an object
* 15948 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L155) a group's _id may only be specified once
* 15949 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L213) 
* 15950 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L224) 
* 15951 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L229) 
* 15952 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L248) 
* 15953 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L263) 
* 15954 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L275) 
* 15955 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L282) a group specification must include an _id
* 15956 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L294) the _id field for a group must not be undefined


src/mongo/db/pipeline/document_source_limit.cpp
----
* 15957 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_limit.cpp#L71) the limit must be specified as a number
* 15958 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_limit.cpp#L78) the limit must be positive


src/mongo/db/pipeline/document_source_match.cpp
----
* 15959 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_match.cpp#L62) the match filter must be an expression in an object


src/mongo/db/pipeline/document_source_project.cpp
----
* 15960 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_project.cpp#L86) 
* 15961 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_project.cpp#L95) 
* 15969 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_project.cpp#L118) 
* 15970 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_project.cpp#L160) 
* 15971 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_project.cpp#L197) 


src/mongo/db/pipeline/document_source_skip.cpp
----
* 15972 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_skip.cpp#L88) the value to 


src/mongo/db/pipeline/document_source_sort.cpp
----
* 15973 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_sort.cpp#L109)  the 
* 15974 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_sort.cpp#L124) 
* 15975 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_sort.cpp#L129) 
* 15976 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_sort.cpp#L137) 


src/mongo/db/pipeline/document_source_unwind.cpp
----
* 15977 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_unwind.cpp#L100) 
* 15978 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_unwind.cpp#L113) 
* 15979 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_unwind.cpp#L204) 
* 15980 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_unwind.cpp#L208) the path of the field to unwind cannot be empty
* 15981 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_unwind.cpp#L221) the 


src/mongo/db/pipeline/expression.cpp
----
* 15982 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L58) 
* 15983 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L90) 
* 15990 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L101) this object is already an operator expression, and can't be used as a document expression (at \"
* 15991 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L145) 
* 15992 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L163) 
* 15993 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2331) 
* 15994 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L789) 
* 15995 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L827) 
* 15996 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2668) cannot subtract one date from another
* 15997 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2338) 
* 15999 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L238) invalid operator \"
* 16000 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L408) can't add two dates together
* 16008 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1304) 
* 16009 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1310) 
* 16010 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1317) 
* 16011 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1332) 
* 16012 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1345) 
* 16013 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1357) 
* 16014 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1577) 
* 16015 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1591) 
* 16019 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L249) the 
* 16020 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L272) the 
* 16021 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L256) the 
* 16022 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L286) the 
* 16034 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2611) 
* 16035 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2617) 


src/mongo/db/pipeline/field_path.cpp
----
* 15998 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/field_path.cpp#L50) 


src/mongo/db/pipeline/value.cpp
----
* 16001 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L86) 
* 16002 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L172) 
* 16003 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L545) 
* 16004 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L571) 
* 16005 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L597) 
* 16006 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L617) 
* 16007 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L653) 
* 16016 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L701) 
* 16017 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L753) 
* 16018 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L851) 


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
* 10111 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L281) table scans not allowed:
* 10112 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L403) bad hint
* 10113 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L415) bad hint
* 10363 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L224) newCursor() with start location not implemented for indexed plans
* 10364 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L245) newReverseCursor() not implemented for indexed plans
* 10365 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L381) 
* 10366 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L440) natural order cannot be specified with $min/$max
* 10367 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L451) 
* 10368 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L507) Unable to locate previously recorded index
* 10369 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L715) no plans
* 13038 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L483) can't find special index: " + _special + " for: 
* 13040 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L104) no type for special: 
* 13268 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L915) invalid $or spec
* 13292 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L390) hint eoo
* 14820 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L299) doing _id query on a capped collection 
* 15878 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L616) 
* 15894 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.cpp#L720) no index matches QueryPlanSet's sort with _bestGuessOnly


src/mongo/db/queryoptimizer.h
----
* 13266 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.h#L476) not implemented for $or query
* 13271 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.h#L479) can't run more ops
* 13335 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.h#L163) yield not supported
* 13336 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizer.h#L165) yield not supported


src/mongo/db/queryoptimizercursor.cpp
----
* 14809 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizercursor.cpp#L443) Invalid access for cursor that is not ok()
* 15940 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizercursor.cpp#L52) 
* 15941 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizercursor.cpp#L288) 
* 15985 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizercursor.cpp#L492) 
* 9011 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryoptimizercursor.cpp#L47) Not an index cursor


src/mongo/db/queryutil-inl.h
----
* 14049 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil-inl.h#L126) FieldRangeSetPair invalid index specified


src/mongo/db/queryutil.cpp
----
* 10370 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L338) $all requires array
* 12580 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L167) invalid query
* 13033 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L710) can't have 2 special fields
* 13034 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L902) invalid use of $not
* 13041 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L911) invalid use of $not
* 13050 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L820) $all requires array
* 13262 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L1445) $or requires nonempty array
* 13263 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L1449) $or array must contain objects
* 13274 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L1461) no or clause to pop
* 13291 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L1451) $or may not contain 'special' query
* 13303 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L1077) combinatorial limit of $in partitioning of result set exceeded
* 13304 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L1087) combinatorial limit of $in partitioning of result set exceeded
* 13385 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L965) combinatorial limit of $in partitioning of result set exceeded
* 13454 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L243) invalid regular expression operator
* 14048 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L1155) FieldRangeSetPair invalid index specified
* 14816 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L858) $and expression must be a nonempty array
* 14817 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L862) $and elements must be objects
* 15881 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L171) $elemMatch not allowed within $in
* 16057 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L1026) 
* 16058 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L940) 
* 16059 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/queryutil.cpp#L933) 


src/mongo/db/repl.cpp
----
* 10002 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L389) local.sources collection corrupt?
* 10118 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L257) 'host' field not set in sources collection object
* 10119 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L258) only source='main' allowed for now with replication", sourceName() == "main
* 10120 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L261) bad sources 'syncedTo' field value
* 10123 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L1000) replication error last applied optime at slave >= nextOpTime from master
* 10124 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L1202) 
* 10384 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L400) --only requires use of --source
* 10385 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L456) Unable to get database list
* 10386 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L772) non Date ts found: 
* 10389 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L801) Unable to get database list
* 10390 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L888) got $err reading remote oplog
* 10391 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L893) repl: bad object read from remote oplog
* 10392 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L1068) bad user object? [1]
* 10393 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L1069) bad user object? [2]
* 13344 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L884) trying to slave off of a non-master
* 14032 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L561) Invalid 'ts' in remote log
* 14033 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L567) Unable to get database list
* 14034 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L609) Duplicate database names present after attempting to delete duplicates
* 15914 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl.cpp#L620) Failure retrying initial sync update


src/mongo/db/repl/health.h
----
* 13112 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/health.h#L41) bad replset heartbeat option
* 13113 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/health.h#L42) bad replset heartbeat option


src/mongo/db/repl/heartbeat.cpp
----
* 15900 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/heartbeat.cpp#L146) can't heartbeat: too much lock


src/mongo/db/repl/rs.cpp
----
* 13093 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs.cpp#L302) bad --replSet config string format is: <setname>[/<seedhost1>,<seedhost2>,...]
* 13096 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs.cpp#L321) bad --replSet command line config string - dups?
* 13101 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs.cpp#L323) can't use localhost in replset host list
* 13114 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs.cpp#L319) bad --replSet seed hostname
* 13290 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs.cpp#L381) bad replSet oplog entry?
* 13302 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs.cpp#L482) replSet error self appears twice in the repl set configuration


src/mongo/db/repl/rs_config.cpp
----
* 13107 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L529) 
* 13108 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L539) bad replset config -- duplicate hosts in the config object?
* 13109 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L645) multiple rows in " << rsConfigNs << " not supported host: 
* 13115 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L460) bad " + rsConfigNs + " config: version
* 13117 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L546) bad " + rsConfigNs + " config
* 13122 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L563) bad repl set config?
* 13126 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L138) bad Member config
* 13131 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L470) replSet error parsing (or missing) 'members' field in config object
* 13132 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L318) 
* 13133 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L322) replSet bad config no members
* 13135 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L535) 
* 13260 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L620) 
* 13308 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L321) replSet bad config version #
* 13309 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L323) replSet bad config maximum number of members is 12
* 13393 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L545) can't use localhost in repl set member names except when using it for all members
* 13419 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L145) priorities must be between 0.0 and 100.0
* 13432 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L270) _id may not change for members
* 13433 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L287) can't find self in new replset config
* 13434 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L42) unexpected field '" << e.fieldName() << "' in object
* 13437 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L146) slaveDelay requires priority be zero
* 13438 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L147) bad slaveDelay value
* 13439 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L148) priority must be 0 when hidden=true
* 13476 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L274) buildIndexes may not change for members
* 13477 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L149) priority must be 0 when buildIndexes=false
* 13510 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L280) arbiterOnly may not change for members
* 13612 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L330) replSet bad config maximum number of voting members is 7
* 13613 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L331) replSet bad config no voting members
* 13645 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L264) hosts cannot switch between localhost and hostname
* 14046 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L380) getLastErrorMode rules must be objects
* 14827 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L521) arbiters cannot have tags
* 14828 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L392) getLastErrorMode criteria must be greater than 0: 
* 14829 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L387) getLastErrorMode criteria must be numeric
* 14831 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L397) mode " << clauseObj << " requires 


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
* 1000 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_sync.cpp#L352) replSet source for syncing doesn't seem to be await capable -- is it an older version of mongodb?
* 12000 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_sync.cpp#L447) rs slaveDelay differential too big check clocks and systems
* 13508 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_sync.cpp#L100) no 'ts' in first op in oplog: 
* 15915 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_sync.cpp#L220) replSet update still fails after adding missing object


src/mongo/db/repl_block.cpp
----
* 14830 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl_block.cpp#L180) unrecognized getLastError mode: 


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
* 16061 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/scanandorder.cpp#L101) 


src/mongo/db/security.cpp
----
* 15889 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/security.cpp#L77) key file must be used to log in with internal user


src/mongo/dbtests/framework.cpp
----
* 10162 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/dbtests/framework.cpp#L409) already have suite with that name


src/mongo/dbtests/jsobjtests.cpp
----
* 12528 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/dbtests/jsobjtests.cpp#L1833) should be ok for storage:
* 12529 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/dbtests/jsobjtests.cpp#L1840) should NOT be ok for storage:


src/mongo/dbtests/queryoptimizertests.cpp
----
* 10408 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/dbtests/queryoptimizertests.cpp#L582) throw
* 10409 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/dbtests/queryoptimizertests.cpp#L621) throw
* 10410 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/dbtests/queryoptimizertests.cpp#L749) throw
* 10411 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/dbtests/queryoptimizertests.cpp#L762) throw


src/mongo/s/balance.cpp
----
* 13258 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/balance.cpp#L304) oids broken after resetting!


src/mongo/s/chunk.cpp
----
* 10163 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L127) can only handle numbers here - which i think is correct
* 10165 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L264) can't split as shard doesn't have a manager
* 10167 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L302) can't move shard to its current location!
* 10169 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L433) datasize failed!" , conn->runCommand( "admin
* 10170 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L67) Chunk needs a ns
* 10171 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L70) Chunk needs a server
* 10172 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L72) Chunk needs a min
* 10173 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L73) Chunk needs a max
* 10174 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L902) config servers not all up
* 10412 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L411) 
* 13003 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L267) can't split a chunk with only one distinct value
* 13141 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L780) Chunk map pointed to incorrect chunk
* 13282 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L592) Couldn't load a valid config for " + _ns + " after 3 attempts. Please try again.
* 13327 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L68) Chunk ns must match server ns
* 13331 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L900) collection's metadata is undergoing changes. Please try again.
* 13332 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L265) need a split key to split chunk
* 13333 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L266) can't split a chunk in that many parts
* 13345 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L189) 
* 13405 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L849) min value " << min << " does not have shard key
* 13406 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L850) max value " << max << " does not have shard key
* 13501 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L804) use geoNear command rather than $near query
* 13502 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L811) unrecognized special query type: 
* 13503 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L160) 
* 13507 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L856) no chunks found between bounds " << min << " and 
* 14022 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L897) Error locking distributed lock for chunk drop.
* 15903 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L740) 
* 8070 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L784) couldn't find a chunk which should be impossible: 
* 8071 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L940) cleaning up after drop failed: 


src/mongo/s/client.cpp
----
* 13134 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/client.cpp#L65) 


src/mongo/s/commands_admin.cpp
----
* 15879 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_admin.cpp#L184) 


src/mongo/s/commands_public.cpp
----
* 10418 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L298) how could chunk manager be null!
* 10420 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L787) how could chunk manager be null!
* 12594 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L550) how could chunk manager be null!
* 13002 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L672) shard internal error chunk manager should never be null
* 13091 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L852) how could chunk manager be null!
* 13092 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L853) GridFS chunks collection can only be sharded on files_id", cm->getShardKey().key() == BSON("files_id
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
* 13500 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L883) how could chunk manager be null!
* 13512 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L301) drop collection attempted on non-sharded collection
* 15920 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L1067) Cannot output to a non-sharded collection, a sharded collection exists


src/mongo/s/config.cpp
----
* 10176 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L534) shard state missing for 
* 10178 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L129) no primary!
* 10181 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L241) not sharded:
* 10184 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L537) _dropShardedCollections too many collections - bailing
* 10187 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L571) need configdbs
* 10189 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L737) should only have 1 thing in config.version
* 13396 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L404) DBConfig save failed: 
* 13449 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L162) collections already sharded
* 13473 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L92) failed to save collection (" + ns + "): 
* 13509 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L354) can't migrate from 1.5.x release to the current one; need to upgrade to 1.6.x first
* 13648 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L149) can't shard collection because not all config servers are up
* 14822 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L311) state changed in the middle: 
* 15883 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L326) not sharded after chunk manager reset : 
* 15885 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L267) not sharded after reloading from chunks : 
* 8042 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L148) db doesn't have sharding enabled
* 8043 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L156) collection already sharded


src/mongo/s/config.h
----
* 10190 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.h#L213) ConfigServer not setup
* 8041 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.h#L158) no primary shard configured for db: 


src/mongo/s/cursors.cpp
----
* 10191 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/cursors.cpp#L75) cursor already done
* 13286 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/cursors.cpp#L218) sent 0 cursors to kill
* 13287 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/cursors.cpp#L219) too many cursors to kill


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
* 15851 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_chunk_manager.cpp#L142) 


src/mongo/s/d_logic.cpp
----
* 10422 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_logic.cpp#L98) write with bad shard config and no server id!
* 9517 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_logic.cpp#L91) writeback


src/mongo/s/d_split.cpp
----
* 13593 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_split.cpp#L777) 


src/mongo/s/d_state.cpp
----
* 13298 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_state.cpp#L77) 
* 13299 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_state.cpp#L99) 
* 13647 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_state.cpp#L551) context should be empty here, is: 


src/mongo/s/grid.cpp
----
* 10185 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/grid.cpp#L96) can't find a shard to put new db on
* 10186 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/grid.cpp#L110) removeDB expects db name
* 10421 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/grid.cpp#L460) getoptime failed" , conn->simpleCommand( "admin" , &result , "getoptime
* 15918 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/grid.cpp#L42) invalid database name: 


src/mongo/s/mr_shard.cpp
----
* 14836 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/mr_shard.cpp#L45) couldn't compile code for: 
* 14837 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/mr_shard.cpp#L149) value too large to reduce
* 14838 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/mr_shard.cpp#L169) reduce -> multiple not supported yet
* 14839 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/mr_shard.cpp#L231) unknown out specifier [" << t << "]
* 14840 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/mr_shard.cpp#L243) 'out' has to be a string or an object
* 14841 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/mr_shard.cpp#L203) outType is no longer a valid option" , cmdObj["outType


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
* 10197 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/server.cpp#L178) createDirectClient not implemented for sharding yet
* 15849 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/server.cpp#L82) client info not defined


src/mongo/s/shard.cpp
----
* 13128 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard.cpp#L134) can't find shard for: 
* 13129 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard.cpp#L116) can't find shard for: 
* 13136 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard.cpp#L338) 
* 13632 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard.cpp#L40) couldn't get updated shard list from config server
* 14807 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard.cpp#L282) no set name for shard: " << _name << " 
* 15847 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard.cpp#L395) can't authenticate to shard server
* 15907 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard.cpp#L407) could not initialize sharding on connection 


src/mongo/s/shard_version.cpp
----
* 10428 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard_version.cpp#L228) need_authoritative set but in authoritative mode already
* 10429 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard_version.cpp#L257) 
* 15904 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard_version.cpp#L79) cannot set version on invalid connection 
* 15905 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard_version.cpp#L84) cannot set version or shard on pair connection 
* 15906 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard_version.cpp#L87) cannot set version or shard on sync connection 


src/mongo/s/shardkey.cpp
----
* 10198 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shardkey.cpp#L46) left object ("  << lObject << ") doesn't have full shard key (
* 10199 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shardkey.cpp#L49) right object (" << rObject << ") doesn't have full shard key (


src/mongo/s/shardkey.h
----
* 13334 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shardkey.h#L120) Shard Key must be less than 512 bytes


src/mongo/s/strategy.cpp
----
* 10200 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy.cpp#L58) mongos: error calling db


src/mongo/s/strategy_shard.cpp
----
* 10201 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L314) invalid update
* 10203 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L413) bad delete message
* 12376 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L370) 
* 13123 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L357) 
* 13465 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L328) shard key in upsert query must be an exact match
* 13505 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L415) $atomic not supported sharded" , pattern["$atomic
* 13506 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L313) $atomic not supported sharded" , query["$atomic
* 14804 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L267) collection no longer sharded
* 14805 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L439) collection no longer sharded
* 14806 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L402) collection no longer sharded
* 16055 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L204) too many retries during bulk insert, " << insertsRemaining.size() << " inserts remaining
* 16056 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L205) shutting down server during bulk insert, " << insertsRemaining.size() << " inserts remaining
* 8010 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L50) something is wrong, shouldn't see a command here
* 8011 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L190) tried to insert object with no valid shard key for " << manager->getShardKey().toString() << " : 
* 8012 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L322) can't upsert something without valid shard key
* 8013 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L338) can't do non-multi update with query that doesn't have a valid shard key
* 8014 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L365) 
* 8015 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L444) can only delete with a non-shard key pattern if can delete as many as we find
* 8016 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_shard.cpp#L480) can't do this write op on sharded collection


src/mongo/s/strategy_single.cpp
----
* 10204 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_single.cpp#L107) dbgrid: getmore: error calling db
* 10205 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_single.cpp#L125) can't use unique indexes with sharding  ns:
* 13390 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_single.cpp#L87) unrecognized command: 
* 8050 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_single.cpp#L146) can't update system.indexes
* 8051 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_single.cpp#L150) can't delete indexes on sharded collection yet
* 8052 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy_single.cpp#L154) handleIndexWrite invalid write op


src/mongo/s/util.h
----
* 13657 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/util.h#L108) unknown type for ShardChunkVersion: 


src/mongo/s/writeback_listener.cpp
----
* 10427 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/writeback_listener.cpp#L163) invalid writeback message
* 13403 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/writeback_listener.cpp#L111) didn't get writeback for: " << oid << " after: " << t.millis() << " ms
* 13641 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/writeback_listener.cpp#L70) can't parse host [" << conn.getServerAddress() << "]
* 14041 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/writeback_listener.cpp#L101) got writeback waitfor for older id 
* 15884 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/writeback_listener.cpp#L223) Could not reload chunk manager after " << attempts << " attempts.


src/mongo/scripting/bench.cpp
----
* 14811 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/bench.cpp#L139) invalid bench dynamic piece: 
* 15930 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/bench.cpp#L693) Authenticating to connection for bench run failed: 
* 15931 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/bench.cpp#L204) Authenticating to connection for _benchThread failed: 
* 15932 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/bench.cpp#L469) Authenticating to connection for benchThread failed: 


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
* 10212 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L83) holder magic value is wrong
* 10213 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L223) non ascii character detected
* 10214 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L251) not a number
* 10215 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L327) not an object
* 10216 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L336) not a function
* 10217 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L393) can't append field.  name:" + name + " type: 
* 10218 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L730) not done: toval
* 10219 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L757) object passed to getPropery is null
* 10220 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L848) don't know what to do with this op
* 10221 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1175) JS_NewRuntime failed
* 10222 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1183) assert not being executed
* 10223 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1258) deleted SMScope twice?
* 10224 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1321) already local connected
* 10225 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1331) already setup for external db
* 10226 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1333) connected to different db
* 10227 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1402) unknown type
* 10228 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1558)  exec failed: 
* 10229 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1757) need a scope
* 10431 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1234) JS_NewContext failed
* 10432 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1241) JS_NewObject failed for global
* 10433 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L1243) js init failed
* 13072 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L36) JS_NewObject failed: 
* 13076 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L153) recursive toObject
* 13498 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L216) 
* 13615 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L40) JS allocation failed, either memory leak or using too much memory
* 9006 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_spidermonkey.cpp#L46) invalid utf8


src/mongo/scripting/engine_v8.cpp
----
* 10230 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L570) can't handle external yet
* 10231 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L615) not an object
* 10232 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L677) not a func
* 10233 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L787) 
* 10234 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L814) 
* 12509 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L578) don't know what this is: 
* 12510 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L879) externalSetup already called, can't call externalSetup
* 12511 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L883) localConnect called with a different name previously
* 12512 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L902) localConnect already called, can't call externalSetup
* 13475 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L797) 


src/mongo/scripting/sm_db.cpp
----
* 10235 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/sm_db.cpp#L78) no cursor!
* 10236 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/sm_db.cpp#L83) no args to internal_cursor_constructor
* 10237 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/sm_db.cpp#L156) mongo_constructor not implemented yet
* 10239 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/sm_db.cpp#L214) no connection!
* 10245 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/sm_db.cpp#L308) no connection!
* 10248 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/sm_db.cpp#L336) no connection!
* 10251 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/sm_db.cpp#L395) no connection!


src/mongo/scripting/utils.cpp
----
* 10261 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/utils.cpp#L29) js md5 needs a string


src/mongo/shell/shell_utils.cpp
----
* 10257 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L128) need to specify 1 argument to listFiles
* 10258 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L109) processinfo not supported
* 12513 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L963) connect failed", scope.exec( _dbConnect , "(connect)
* 12514 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L966) login failed", scope.exec( _dbAuth , "(auth)
* 12518 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L877) srand requires a single numeric argument
* 12519 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L884) rand accepts no arguments
* 12581 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L137) 
* 12597 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L205) need to specify 1 argument
* 13006 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L895) isWindows accepts no arguments
* 13301 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L225) cat() : file to big to load as a variable
* 13411 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L911) getHostName accepts no arguments
* 13619 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L276) fuzzFile takes 2 arguments
* 13620 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L279) couldn't open file to fuzz
* 13621 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L650) no known mongo program on port
* 14042 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L557) 
* 15852 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L844) stopMongoByPid needs a number
* 15853 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L835) stopMongo needs a number


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
* 15934 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/restore.cpp#L430) JSON object size didn't match file size
* 15935 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/restore.cpp#L84) user does not have write access
* 15936 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/restore.cpp#L494) Creating collection " + _curns + " failed. Errmsg: " + info["errmsg


src/mongo/tools/sniffer.cpp
----
* 10266 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/sniffer.cpp#L482) can't use --source twice
* 10267 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/sniffer.cpp#L483) source needs more args


src/mongo/tools/tool.cpp
----
* 10264 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/tool.cpp#L493) invalid object size: 
* 10265 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/tool.cpp#L529) counts don't match
* 9997 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/tool.cpp#L432) authentication failed: 
* 9998 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/tool.cpp#L395) you need to specify fields
* 9999 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/tool.cpp#L374) file: " + fn ) + " doesn't exist


src/mongo/util/alignedbuilder.cpp
----
* 13524 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/alignedbuilder.cpp#L109) out of memory AlignedBuilder
* 13584 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/alignedbuilder.cpp#L27) out of memory AlignedBuilder


src/mongo/util/assert_util.h
----
* 10437 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L247) unknown boost failed
* 123 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L74) blah
* 13294 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L245) 
* 14043 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L256) 
* 14044 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L258) unknown boost failed 


src/mongo/util/background.cpp
----
* 13643 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/background.cpp#L52) backgroundjob already started: 


src/mongo/util/base64.cpp
----
* 10270 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/base64.cpp#L79) invalid base64


src/mongo/util/concurrency/list.h
----
* 14050 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/list.h#L84) List1: item to orphan not in list


src/mongo/util/file.h
----
* 10438 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file.h#L117) ReadFile error - truncated file?


src/mongo/util/file_allocator.cpp
----
* 10439 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L257) 
* 10440 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L159) 
* 10441 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L163) Unable to allocate new file of size 
* 10442 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L165) Unable to allocate new file of size 
* 10443 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L180) FileAllocator: file write failed
* 13653 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L276) 


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
* 15870 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L81) 
* 15871 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L84) Couldn't truncate file: 
* 15872 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L187) 
* 15873 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L192) Couldn't truncate file: 


src/mongo/util/mmap.cpp
----
* 13468 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap.cpp#L36) can't create file already exists 
* 13617 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap.cpp#L188) MongoFile : multiple opens of same filename
* 15922 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap.cpp#L60) couldn't get file length when opening mapping 
* 15923 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap.cpp#L70) couldn't get file length when opening mapping 


src/mongo/util/mmap_posix.cpp
----
* 10446 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap_posix.cpp#L80) mmap: can't map area of size 0 file: 
* 10447 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap_posix.cpp#L90) map file alloc failed, wanted: " << length << " filelen: 


src/mongo/util/mmap_win.cpp
----
* 13056 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap_win.cpp#L193) Async flushing not supported on windows


src/mongo/util/net/hostandport.h
----
* 13095 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/hostandport.h#L216) HostAndPort: bad port #
* 13110 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/hostandport.h#L210) HostAndPort: bad host:port config string


src/mongo/util/net/httpclient.cpp
----
* 10271 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/httpclient.cpp#L47) invalid url" , url.find( "http://
* 15862 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/httpclient.cpp#L108) no ssl support


src/mongo/util/net/listen.cpp
----
* 15863 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/listen.cpp#L133) listen(): invalid socket? 


src/mongo/util/net/message.h
----
* 13273 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/message.h#L177) single data buffer expected


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
* 15887 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/message_server_port.cpp#L144) 


src/mongo/util/net/sock.cpp
----
* 13079 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L167) path to unix socket too long
* 13080 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L165) no unix socket support on windows
* 13082 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L253) getnameinfo error 
* 15861 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L499) can't create SSL
* 15864 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L457) can't create SSL Context: 
* 15865 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L465) 
* 15866 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L471) 
* 15867 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L488) Can't read certificate file
* 15868 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L493) Can't read key file


src/mongo/util/paths.h
----
* 13600 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/paths.h#L59) 
* 13646 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/paths.h#L88) stat() failed for file: " << path << " 
* 13650 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/paths.h#L116) Couldn't open directory '" << dir.string() << "' for flushing: 
* 13651 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/paths.h#L120) Couldn't fsync directory '" << dir.string() << "': 
* 13652 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/paths.h#L104) Couldn't find parent dir for file: 


src/mongo/util/processinfo_linux2.cpp
----
* 13538 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/processinfo_linux2.cpp#L45) 


src/mongo/util/processinfo_win32.cpp
----
* 16050 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/processinfo_win32.cpp#L48) 
* 16051 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/processinfo_win32.cpp#L56) 


src/mongo/util/text.h
----
* 13305 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/text.h#L131) could not convert string to long long
* 13306 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/text.h#L140) could not convert string to long long
* 13307 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/text.h#L126) cannot convert empty string to long long
* 13310 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/text.h#L144) could not convert string to long long

