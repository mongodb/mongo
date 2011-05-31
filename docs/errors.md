MongoDB Error Codes
==========




bson/bson-inl.h
----
* 10065 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L37) 
* 10313 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L397) Insufficient bytes to calculate element size
* 10314 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L401) Insufficient bytes to calculate element size
* 10315 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L406) Insufficient bytes to calculate element size
* 10316 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L411) Insufficient bytes to calculate element size
* 10317 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L415) Insufficient bytes to calculate element size
* 10318 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L421) Invalid regex string
* 10319 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L431) Invalid regex options string
* 10320 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L505) 
* 10321 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L342) 
* 10322 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L347) Invalid CodeWScope size
* 10323 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L349) Invalid CodeWScope string size
* 10324 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L350) Invalid CodeWScope string size
* 10325 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L353) Invalid CodeWScope size
* 10326 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L355) Invalid CodeWScope object size
* 10327 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L302) Object does not end with EOO
* 10328 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L304) Invalid element size
* 10329 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L305) Element too large
* 10330 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L307) Element extends past end of object
* 10331 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L312) EOO Before end of object
* 10334 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L61) 
* 13655 [code](http://github.com/mongodb/mongo/blob/master/bson/bson-inl.h#L439) 


bson/bson_db.h
----
* 10062 [code](http://github.com/mongodb/mongo/blob/master/bson/bson_db.h#L60) not code


bson/bsonelement.h
----
* 10063 [code](http://github.com/mongodb/mongo/blob/master/bson/bsonelement.h#L357) not a dbref
* 10064 [code](http://github.com/mongodb/mongo/blob/master/bson/bsonelement.h#L362) not a dbref
* 10333 [code](http://github.com/mongodb/mongo/blob/master/bson/bsonelement.h#L386) Invalid field name
* 13111 [code](http://github.com/mongodb/mongo/blob/master/bson/bsonelement.h#L423) 
* 13118 [code](http://github.com/mongodb/mongo/blob/master/bson/bsonelement.h#L428) unexpected or missing type value in BSON object


bson/bsonobjbuilder.h
----
* 10335 [code](http://github.com/mongodb/mongo/blob/master/bson/bsonobjbuilder.h#L538) builder does not own memory
* 10336 [code](http://github.com/mongodb/mongo/blob/master/bson/bsonobjbuilder.h#L613) No subobject started
* 13048 [code](http://github.com/mongodb/mongo/blob/master/bson/bsonobjbuilder.h#L754) can't append to array using string field name [" + name.data() + "]


bson/ordering.h
----
* 13103 [code](http://github.com/mongodb/mongo/blob/master/bson/ordering.h#L57) too many compound keys


bson/util/builder.h
----
* 10000 [code](http://github.com/mongodb/mongo/blob/master/bson/util/builder.h#L92) out of memory BufBuilder
* 13548 [code](http://github.com/mongodb/mongo/blob/master/bson/util/builder.h#L202) BufBuilder grow() > 64MB


client/clientOnly.cpp
----
* 10256 [code](http://github.com/mongodb/mongo/blob/master/client/clientOnly.cpp#L62) no createDirectClient in clientOnly


client/connpool.cpp
----
* 13071 [code](http://github.com/mongodb/mongo/blob/master/client/connpool.cpp#L171) invalid hostname [" + host + "]
* 13328 [code](http://github.com/mongodb/mongo/blob/master/client/connpool.cpp#L157) : connect failed " + url.toString() + " : 


client/connpool.h
----
* 11004 [code](http://github.com/mongodb/mongo/blob/master/client/connpool.h#L213) connection was returned to the pool already
* 11005 [code](http://github.com/mongodb/mongo/blob/master/client/connpool.h#L219) connection was returned to the pool already
* 13102 [code](http://github.com/mongodb/mongo/blob/master/client/connpool.h#L225) connection was returned to the pool already


client/dbclient.cpp
----
* 10005 [code](http://github.com/mongodb/mongo/blob/master/client/dbclient.cpp#L479) listdatabases failed" , runCommand( "admin" , BSON( "listDatabases
* 10006 [code](http://github.com/mongodb/mongo/blob/master/client/dbclient.cpp#L480) listDatabases.databases not array" , info["databases
* 10007 [code](http://github.com/mongodb/mongo/blob/master/client/dbclient.cpp#L788) dropIndex failed
* 10008 [code](http://github.com/mongodb/mongo/blob/master/client/dbclient.cpp#L795) dropIndexes failed
* 10276 [code](http://github.com/mongodb/mongo/blob/master/client/dbclient.cpp#L541) DBClientBase::findN: transport error: " << getServerAddress() << " query: 
* 10278 [code](http://github.com/mongodb/mongo/blob/master/client/dbclient.cpp#L929) dbclient error communicating with server: 
* 10337 [code](http://github.com/mongodb/mongo/blob/master/client/dbclient.cpp#L881) object not valid
* 11010 [code](http://github.com/mongodb/mongo/blob/master/client/dbclient.cpp#L281) count fails:
* 13386 [code](http://github.com/mongodb/mongo/blob/master/client/dbclient.cpp#L661) socket error for mapping query
* 13421 [code](http://github.com/mongodb/mongo/blob/master/client/dbclient.cpp#L100) trying to connect to invalid ConnectionString


client/dbclient.h
----
* 10011 [code](http://github.com/mongodb/mongo/blob/master/client/dbclient.h#L498) no collection name
* 9000 [code](http://github.com/mongodb/mongo/blob/master/client/dbclient.h#L812) 


client/dbclient_rs.cpp
----
* 10009 [code](http://github.com/mongodb/mongo/blob/master/client/dbclient_rs.cpp#L208) ReplicaSetMonitor no master found for set: 
* 13610 [code](http://github.com/mongodb/mongo/blob/master/client/dbclient_rs.cpp#L156) ConfigChangeHook already specified
* 13639 [code](http://github.com/mongodb/mongo/blob/master/client/dbclient_rs.cpp#L453) can't connect to new replica set master [" << _masterHost.toString() << "] err: 
* 13642 [code](http://github.com/mongodb/mongo/blob/master/client/dbclient_rs.cpp#L79) need at least 1 node for a replica set


client/dbclientcursor.cpp
----
* 13127 [code](http://github.com/mongodb/mongo/blob/master/client/dbclientcursor.cpp#L148) getMore: cursor didn't exist on server, possible restart or timeout?
* 13422 [code](http://github.com/mongodb/mongo/blob/master/client/dbclientcursor.cpp#L196) DBClientCursor next() called but more() is false


client/dbclientcursor.h
----
* 13106 [code](http://github.com/mongodb/mongo/blob/master/client/dbclientcursor.h#L78) 
* 13348 [code](http://github.com/mongodb/mongo/blob/master/client/dbclientcursor.h#L202) connection died
* 13383 [code](http://github.com/mongodb/mongo/blob/master/client/dbclientcursor.h#L219) BatchIterator empty


client/distlock.cpp
----
* 14023 [code](http://github.com/mongodb/mongo/blob/master/client/distlock.cpp#L582) remote time in cluster " << _conn.toString() << " is now skewed, cannot force lock.


client/distlock_test.cpp
----
* 13678 [code](http://github.com/mongodb/mongo/blob/master/client/distlock_test.cpp#L374) Could not communicate with server " << server.toString() << " in cluster " << cluster.toString() << " to change skew by 


client/gridfs.cpp
----
* 10012 [code](http://github.com/mongodb/mongo/blob/master/client/gridfs.cpp#L90) file doesn't exist" , fileName == "-
* 10013 [code](http://github.com/mongodb/mongo/blob/master/client/gridfs.cpp#L97) error opening file
* 10014 [code](http://github.com/mongodb/mongo/blob/master/client/gridfs.cpp#L210) chunk is empty!
* 10015 [code](http://github.com/mongodb/mongo/blob/master/client/gridfs.cpp#L242) doesn't exists
* 13296 [code](http://github.com/mongodb/mongo/blob/master/client/gridfs.cpp#L64) invalid chunk size is specified
* 13325 [code](http://github.com/mongodb/mongo/blob/master/client/gridfs.cpp#L236) couldn't open file: 
* 9008 [code](http://github.com/mongodb/mongo/blob/master/client/gridfs.cpp#L136) filemd5 failed


client/model.cpp
----
* 10016 [code](http://github.com/mongodb/mongo/blob/master/client/model.cpp#L39) _id isn't set - needed for remove()" , _id["_id
* 13121 [code](http://github.com/mongodb/mongo/blob/master/client/model.cpp#L81) 
* 9002 [code](http://github.com/mongodb/mongo/blob/master/client/model.cpp#L51) error on Model::remove: 
* 9003 [code](http://github.com/mongodb/mongo/blob/master/client/model.cpp#L123) error on Model::save: 


client/parallel.cpp
----
* 10017 [code](http://github.com/mongodb/mongo/blob/master/client/parallel.cpp#L80) cursor already done
* 10018 [code](http://github.com/mongodb/mongo/blob/master/client/parallel.cpp#L335) no more items
* 10019 [code](http://github.com/mongodb/mongo/blob/master/client/parallel.cpp#L534) no more elements
* 13431 [code](http://github.com/mongodb/mongo/blob/master/client/parallel.cpp#L395) have to have sort key in projection and removing it
* 13633 [code](http://github.com/mongodb/mongo/blob/master/client/parallel.cpp#L109) error querying server: 
* 14047 [code](http://github.com/mongodb/mongo/blob/master/client/parallel.cpp#L470) error querying server: 


client/syncclusterconnection.cpp
----
* 10022 [code](http://github.com/mongodb/mongo/blob/master/client/syncclusterconnection.cpp#L238) SyncClusterConnection::getMore not supported yet
* 10023 [code](http://github.com/mongodb/mongo/blob/master/client/syncclusterconnection.cpp#L260) SyncClusterConnection bulk insert not implemented
* 13053 [code](http://github.com/mongodb/mongo/blob/master/client/syncclusterconnection.cpp#L375) help failed: " << info , _commandOnActive( "admin" , BSON( name << "1" << "help
* 13054 [code](http://github.com/mongodb/mongo/blob/master/client/syncclusterconnection.cpp#L204) write $cmd not supported in SyncClusterConnection::query for:
* 13104 [code](http://github.com/mongodb/mongo/blob/master/client/syncclusterconnection.cpp#L169) SyncClusterConnection::findOne prepare failed: 
* 13105 [code](http://github.com/mongodb/mongo/blob/master/client/syncclusterconnection.cpp#L187) 
* 13119 [code](http://github.com/mongodb/mongo/blob/master/client/syncclusterconnection.cpp#L245) SyncClusterConnection::insert obj has to have an _id: 
* 13120 [code](http://github.com/mongodb/mongo/blob/master/client/syncclusterconnection.cpp#L278) SyncClusterConnection::update upsert query needs _id" , query.obj["_id
* 13397 [code](http://github.com/mongodb/mongo/blob/master/client/syncclusterconnection.cpp#L353) SyncClusterConnection::say prepare failed: 
* 8001 [code](http://github.com/mongodb/mongo/blob/master/client/syncclusterconnection.cpp#L135) SyncClusterConnection write op failed: 
* 8002 [code](http://github.com/mongodb/mongo/blob/master/client/syncclusterconnection.cpp#L234) all servers down!
* 8003 [code](http://github.com/mongodb/mongo/blob/master/client/syncclusterconnection.cpp#L250) SyncClusterConnection::insert prepare failed: 
* 8004 [code](http://github.com/mongodb/mongo/blob/master/client/syncclusterconnection.cpp#L50) SyncClusterConnection needs 3 servers
* 8005 [code](http://github.com/mongodb/mongo/blob/master/client/syncclusterconnection.cpp#L284) SyncClusterConnection::udpate prepare failed: 
* 8006 [code](http://github.com/mongodb/mongo/blob/master/client/syncclusterconnection.cpp#L327) SyncClusterConnection::call can only be used directly for dbQuery
* 8007 [code](http://github.com/mongodb/mongo/blob/master/client/syncclusterconnection.cpp#L331) SyncClusterConnection::call can't handle $cmd" , strstr( d.getns(), "$cmd
* 8008 [code](http://github.com/mongodb/mongo/blob/master/client/syncclusterconnection.cpp#L347) all servers down!
* 8020 [code](http://github.com/mongodb/mongo/blob/master/client/syncclusterconnection.cpp#L266) SyncClusterConnection::remove prepare failed: 


db/btree.cpp
----
* 10281 [code](http://github.com/mongodb/mongo/blob/master/db/btree.cpp#L138) assert is misdefined
* 10282 [code](http://github.com/mongodb/mongo/blob/master/db/btree.cpp#L319) n==0 in btree popBack()
* 10283 [code](http://github.com/mongodb/mongo/blob/master/db/btree.cpp#L326) rchild not null in btree popBack()
* 10285 [code](http://github.com/mongodb/mongo/blob/master/db/btree.cpp#L1695) _insert: reuse key but lchild is not this->null
* 10286 [code](http://github.com/mongodb/mongo/blob/master/db/btree.cpp#L1696) _insert: reuse key but rchild is not this->null


db/btree.h
----
* 13000 [code](http://github.com/mongodb/mongo/blob/master/db/btree.h#L279) invalid keyNode: " +  BSON( "i" << i << "n


db/btreebuilder.cpp
----
* 10288 [code](http://github.com/mongodb/mongo/blob/master/db/btreebuilder.cpp#L76) bad key order in BtreeBuilder - server internal error


db/btreecursor.cpp
----
* 13384 [code](http://github.com/mongodb/mongo/blob/master/db/btreecursor.cpp#L222) BtreeCursor FieldRangeVector constructor doesn't accept special indexes
* 14800 [code](http://github.com/mongodb/mongo/blob/master/db/btreecursor.cpp#L171) unsupported index version 
* 14801 [code](http://github.com/mongodb/mongo/blob/master/db/btreecursor.cpp#L187) unsupported index version 


db/cap.cpp
----
* 10345 [code](http://github.com/mongodb/mongo/blob/master/db/cap.cpp#L253) passes >= maxPasses in capped collection alloc
* 13415 [code](http://github.com/mongodb/mongo/blob/master/db/cap.cpp#L339) emptying the collection is not allowed
* 13424 [code](http://github.com/mongodb/mongo/blob/master/db/cap.cpp#L406) collection must be capped
* 13425 [code](http://github.com/mongodb/mongo/blob/master/db/cap.cpp#L407) background index build in progress
* 13426 [code](http://github.com/mongodb/mongo/blob/master/db/cap.cpp#L408) indexes present


db/client.cpp
----
* 10057 [code](http://github.com/mongodb/mongo/blob/master/db/client.cpp#L254) 
* 13005 [code](http://github.com/mongodb/mongo/blob/master/db/client.cpp#L221) can't create db, keeps getting closed
* 14031 [code](http://github.com/mongodb/mongo/blob/master/db/client.cpp#L181) Can't take a write lock while out of disk space


db/client.h
----
* 12600 [code](http://github.com/mongodb/mongo/blob/master/db/client.h#L228) releaseAndWriteLock: unlock_shared failed, probably recursive


db/clientcursor.h
----
* 12051 [code](http://github.com/mongodb/mongo/blob/master/db/clientcursor.h#L108) clientcursor already in use? driver problem?
* 12521 [code](http://github.com/mongodb/mongo/blob/master/db/clientcursor.h#L279) internal error: use of an unlocked ClientCursor


db/cloner.cpp
----
* 10024 [code](http://github.com/mongodb/mongo/blob/master/db/cloner.cpp#L87) bad ns field for index during dbcopy
* 10025 [code](http://github.com/mongodb/mongo/blob/master/db/cloner.cpp#L89) bad ns field for index during dbcopy [2]
* 10026 [code](http://github.com/mongodb/mongo/blob/master/db/cloner.cpp#L640) source namespace does not exist
* 10027 [code](http://github.com/mongodb/mongo/blob/master/db/cloner.cpp#L650) target namespace exists", cmdObj["dropTarget
* 10289 [code](http://github.com/mongodb/mongo/blob/master/db/cloner.cpp#L283) useReplAuth is not written to replication log
* 10290 [code](http://github.com/mongodb/mongo/blob/master/db/cloner.cpp#L354) 
* 13008 [code](http://github.com/mongodb/mongo/blob/master/db/cloner.cpp#L593) must call copydbgetnonce first


db/cmdline.cpp
----
* 10033 [code](http://github.com/mongodb/mongo/blob/master/db/cmdline.cpp#L244) logpath has to be non-zero


db/commands/distinct.cpp
----
* 10044 [code](http://github.com/mongodb/mongo/blob/master/db/commands/distinct.cpp#L115) distinct too big, 16mb cap


db/commands/find_and_modify.cpp
----
* 12515 [code](http://github.com/mongodb/mongo/blob/master/db/commands/find_and_modify.cpp#L87) can't remove and update", cmdObj["update
* 12516 [code](http://github.com/mongodb/mongo/blob/master/db/commands/find_and_modify.cpp#L119) must specify remove or update
* 13329 [code](http://github.com/mongodb/mongo/blob/master/db/commands/find_and_modify.cpp#L64) upsert mode requires update field
* 13330 [code](http://github.com/mongodb/mongo/blob/master/db/commands/find_and_modify.cpp#L65) upsert mode requires query field


db/commands/group.cpp
----
* 10041 [code](http://github.com/mongodb/mongo/blob/master/db/commands/group.cpp#L41) invoke failed in $keyf: 
* 10042 [code](http://github.com/mongodb/mongo/blob/master/db/commands/group.cpp#L43) return of $key has to be an object
* 10043 [code](http://github.com/mongodb/mongo/blob/master/db/commands/group.cpp#L110) group() can't handle more than 20000 unique keys
* 9010 [code](http://github.com/mongodb/mongo/blob/master/db/commands/group.cpp#L116) reduce invoke failed: 


db/commands/isself.cpp
----
* 13469 [code](http://github.com/mongodb/mongo/blob/master/db/commands/isself.cpp#L25) getifaddrs failure: 
* 13470 [code](http://github.com/mongodb/mongo/blob/master/db/commands/isself.cpp#L42) getnameinfo() failed: 
* 13472 [code](http://github.com/mongodb/mongo/blob/master/db/commands/isself.cpp#L88) getnameinfo() failed: 


db/commands/mr.cpp
----
* 10074 [code](http://github.com/mongodb/mongo/blob/master/db/commands/mr.cpp#L155) need values
* 10075 [code](http://github.com/mongodb/mongo/blob/master/db/commands/mr.cpp#L196) reduce -> multiple not supported yet
* 10076 [code](http://github.com/mongodb/mongo/blob/master/db/commands/mr.cpp#L421) rename failed: 
* 10077 [code](http://github.com/mongodb/mongo/blob/master/db/commands/mr.cpp#L877) fast_emit takes 2 args
* 10078 [code](http://github.com/mongodb/mongo/blob/master/db/commands/mr.cpp#L1131) something bad happened" , shardedOutputCollection == res["result
* 13069 [code](http://github.com/mongodb/mongo/blob/master/db/commands/mr.cpp#L878) an emit can't be more than half max bson size
* 13070 [code](http://github.com/mongodb/mongo/blob/master/db/commands/mr.cpp#L176) value too large to reduce
* 13522 [code](http://github.com/mongodb/mongo/blob/master/db/commands/mr.cpp#L258) unknown out specifier [" << t << "]
* 13598 [code](http://github.com/mongodb/mongo/blob/master/db/commands/mr.cpp#L55) couldn't compile code for: 
* 13602 [code](http://github.com/mongodb/mongo/blob/master/db/commands/mr.cpp#L230) outType is no longer a valid option" , cmdObj["outType
* 13604 [code](http://github.com/mongodb/mongo/blob/master/db/commands/mr.cpp#L378) too much data for in memory map/reduce
* 13605 [code](http://github.com/mongodb/mongo/blob/master/db/commands/mr.cpp#L398) too much data for in memory map/reduce
* 13606 [code](http://github.com/mongodb/mongo/blob/master/db/commands/mr.cpp#L266) 'out' has to be a string or an object
* 13608 [code](http://github.com/mongodb/mongo/blob/master/db/commands/mr.cpp#L300) query has to be blank or an Object
* 13609 [code](http://github.com/mongodb/mongo/blob/master/db/commands/mr.cpp#L307) sort has to be blank or an Object
* 13630 [code](http://github.com/mongodb/mongo/blob/master/db/commands/mr.cpp#L331) userCreateNS failed for mr tempLong ns: " << _config.tempLong << " err: 
* 13631 [code](http://github.com/mongodb/mongo/blob/master/db/commands/mr.cpp#L568) userCreateNS failed for mr incLong ns: " << _config.incLong << " err: 
* 9014 [code](http://github.com/mongodb/mongo/blob/master/db/commands/mr.cpp#L73) map invoke failed: 


db/common.cpp
----
* 10332 [code](http://github.com/mongodb/mongo/blob/master/db/common.cpp#L38) Expected CurrentTime type


db/compact.cpp
----
* 13660 [code](http://github.com/mongodb/mongo/blob/master/db/compact.cpp#L243) namespace " << ns << " does not exist
* 13661 [code](http://github.com/mongodb/mongo/blob/master/db/compact.cpp#L244) cannot compact capped collection
* 14024 [code](http://github.com/mongodb/mongo/blob/master/db/compact.cpp#L86) compact error out of space during compaction
* 14025 [code](http://github.com/mongodb/mongo/blob/master/db/compact.cpp#L183) compact error no space available to allocate
* 14027 [code](http://github.com/mongodb/mongo/blob/master/db/compact.cpp#L235) can't compact a system namespace", !str::contains(ns, ".system.
* 14028 [code](http://github.com/mongodb/mongo/blob/master/db/compact.cpp#L234) bad ns


db/concurrency.h
----
* 13142 [code](http://github.com/mongodb/mongo/blob/master/db/concurrency.h#L134) timeout getting readlock


db/curop.h
----
* 11600 [code](http://github.com/mongodb/mongo/blob/master/db/curop.h#L365) interrupted at shutdown
* 11601 [code](http://github.com/mongodb/mongo/blob/master/db/curop.h#L367) interrupted
* 12601 [code](http://github.com/mongodb/mongo/blob/master/db/curop.h#L256) CurOp not marked done yet


db/cursor.h
----
* 13285 [code](http://github.com/mongodb/mongo/blob/master/db/cursor.h#L133) manual matcher config not allowed


db/database.cpp
----
* 10028 [code](http://github.com/mongodb/mongo/blob/master/db/database.cpp#L47) db name is empty
* 10029 [code](http://github.com/mongodb/mongo/blob/master/db/database.cpp#L48) bad db name [1]
* 10030 [code](http://github.com/mongodb/mongo/blob/master/db/database.cpp#L49) bad db name [2]
* 10031 [code](http://github.com/mongodb/mongo/blob/master/db/database.cpp#L50) bad char(s) in db name
* 10032 [code](http://github.com/mongodb/mongo/blob/master/db/database.cpp#L51) db name too long
* 10295 [code](http://github.com/mongodb/mongo/blob/master/db/database.cpp#L151) getFile(): bad file number value (corrupt db?): run repair
* 12501 [code](http://github.com/mongodb/mongo/blob/master/db/database.cpp#L211) quota exceeded
* 14810 [code](http://github.com/mongodb/mongo/blob/master/db/database.cpp#L224) couldn't allocate space (suitableFile)


db/db.cpp
----
* 10296 [code](http://github.com/mongodb/mongo/blob/master/db/db.cpp#L436) 
* 10297 [code](http://github.com/mongodb/mongo/blob/master/db/db.cpp#L1185) Couldn't register Windows Ctrl-C handler
* 12590 [code](http://github.com/mongodb/mongo/blob/master/db/db.cpp#L441) 
* 14026 [code](http://github.com/mongodb/mongo/blob/master/db/db.cpp#L289) 


db/db.h
----
* 10298 [code](http://github.com/mongodb/mongo/blob/master/db/db.h#L150) can't temprelease nested write lock
* 10299 [code](http://github.com/mongodb/mongo/blob/master/db/db.h#L155) can't temprelease nested read lock
* 13074 [code](http://github.com/mongodb/mongo/blob/master/db/db.h#L126) db name can't be empty
* 13075 [code](http://github.com/mongodb/mongo/blob/master/db/db.h#L129) db name can't be empty
* 13280 [code](http://github.com/mongodb/mongo/blob/master/db/db.h#L119) invalid db name: 


db/dbcommands.cpp
----
* 10039 [code](http://github.com/mongodb/mongo/blob/master/db/dbcommands.cpp#L710) can't drop collection with reserved $ character in name
* 10040 [code](http://github.com/mongodb/mongo/blob/master/db/dbcommands.cpp#L1017) chunks out of order
* 10301 [code](http://github.com/mongodb/mongo/blob/master/db/dbcommands.cpp#L1352) source collection " + fromNs + " does not exist
* 13049 [code](http://github.com/mongodb/mongo/blob/master/db/dbcommands.cpp#L1483) godinsert must specify a collection
* 13281 [code](http://github.com/mongodb/mongo/blob/master/db/dbcommands.cpp#L1036) File deleted during filemd5 command
* 13416 [code](http://github.com/mongodb/mongo/blob/master/db/dbcommands.cpp#L1615) captrunc must specify a collection
* 13417 [code](http://github.com/mongodb/mongo/blob/master/db/dbcommands.cpp#L1623) captrunc collection not found or empty
* 13418 [code](http://github.com/mongodb/mongo/blob/master/db/dbcommands.cpp#L1625) captrunc invalid n
* 13428 [code](http://github.com/mongodb/mongo/blob/master/db/dbcommands.cpp#L1642) emptycapped must specify a collection
* 13429 [code](http://github.com/mongodb/mongo/blob/master/db/dbcommands.cpp#L1645) emptycapped no such collection


db/dbcommands_admin.cpp
----
* 12032 [code](http://github.com/mongodb/mongo/blob/master/db/dbcommands_admin.cpp#L474) fsync: sync option must be true when using lock
* 12033 [code](http://github.com/mongodb/mongo/blob/master/db/dbcommands_admin.cpp#L480) fsync: profiling must be off to enter locked mode
* 12034 [code](http://github.com/mongodb/mongo/blob/master/db/dbcommands_admin.cpp#L473) fsync: can't lock while an unlock is pending


db/dbcommands_generic.cpp
----
* 10038 [code](http://github.com/mongodb/mongo/blob/master/db/dbcommands_generic.cpp#L329) forced error


db/dbeval.cpp
----
* 10046 [code](http://github.com/mongodb/mongo/blob/master/db/dbeval.cpp#L42) eval needs Code
* 12598 [code](http://github.com/mongodb/mongo/blob/master/db/dbeval.cpp#L127) $eval reads unauthorized


db/dbhelpers.cpp
----
* 10303 [code](http://github.com/mongodb/mongo/blob/master/db/dbhelpers.cpp#L319) {autoIndexId:false}
* 13430 [code](http://github.com/mongodb/mongo/blob/master/db/dbhelpers.cpp#L160) no _id index


db/dbmessage.h
----
* 10304 [code](http://github.com/mongodb/mongo/blob/master/db/dbmessage.h#L151) Client Error: Remaining data too small for BSON object
* 10305 [code](http://github.com/mongodb/mongo/blob/master/db/dbmessage.h#L153) Client Error: Invalid object size
* 10306 [code](http://github.com/mongodb/mongo/blob/master/db/dbmessage.h#L154) Client Error: Next object larger than space left in message
* 10307 [code](http://github.com/mongodb/mongo/blob/master/db/dbmessage.h#L157) Client Error: bad object in message
* 13066 [code](http://github.com/mongodb/mongo/blob/master/db/dbmessage.h#L149) Message contains no documents


db/dbwebserver.cpp
----
* 13453 [code](http://github.com/mongodb/mongo/blob/master/db/dbwebserver.cpp#L171) server not started with --jsonp


db/dur.cpp
----
* 13599 [code](http://github.com/mongodb/mongo/blob/master/db/dur.cpp#L370) Written data does not match in-memory view. Missing WriteIntent?
* 13616 [code](http://github.com/mongodb/mongo/blob/master/db/dur.cpp#L192) can't disable durability with pending writes


db/dur_journal.cpp
----
* 13611 [code](http://github.com/mongodb/mongo/blob/master/db/dur_journal.cpp#L499) can't read lsn file in journal directory : 
* 13614 [code](http://github.com/mongodb/mongo/blob/master/db/dur_journal.cpp#L460) unexpected version number of lsn file in journal/ directory got: 


db/dur_recover.cpp
----
* 13531 [code](http://github.com/mongodb/mongo/blob/master/db/dur_recover.cpp#L76) unexpected files in journal directory " << dir.string() << " : 
* 13532 [code](http://github.com/mongodb/mongo/blob/master/db/dur_recover.cpp#L83) 
* 13533 [code](http://github.com/mongodb/mongo/blob/master/db/dur_recover.cpp#L145) problem processing journal file during recovery
* 13535 [code](http://github.com/mongodb/mongo/blob/master/db/dur_recover.cpp#L387) recover abrupt journal file end
* 13536 [code](http://github.com/mongodb/mongo/blob/master/db/dur_recover.cpp#L329) journal version number mismatch 
* 13537 [code](http://github.com/mongodb/mongo/blob/master/db/dur_recover.cpp#L331) journal header invalid
* 13544 [code](http://github.com/mongodb/mongo/blob/master/db/dur_recover.cpp#L369) recover error couldn't open 
* 13545 [code](http://github.com/mongodb/mongo/blob/master/db/dur_recover.cpp#L394) --durOptions " << (int) CmdLine::DurScanOnly << " (scan only) specified
* 13594 [code](http://github.com/mongodb/mongo/blob/master/db/dur_recover.cpp#L125) journal checksum doesn't match
* 13622 [code](http://github.com/mongodb/mongo/blob/master/db/dur_recover.cpp#L240) Trying to write past end of file in WRITETODATAFILES


db/durop.cpp
----
* 13546 [code](http://github.com/mongodb/mongo/blob/master/db/durop.cpp#L51) journal recover: unrecognized opcode in journal 
* 13547 [code](http://github.com/mongodb/mongo/blob/master/db/durop.cpp#L142) recover couldn't create file 
* 13628 [code](http://github.com/mongodb/mongo/blob/master/db/durop.cpp#L156) recover failure writing file 


db/extsort.cpp
----
* 10048 [code](http://github.com/mongodb/mongo/blob/master/db/extsort.cpp#L69) already sorted
* 10049 [code](http://github.com/mongodb/mongo/blob/master/db/extsort.cpp#L94) sorted already
* 10050 [code](http://github.com/mongodb/mongo/blob/master/db/extsort.cpp#L115) bad
* 10308 [code](http://github.com/mongodb/mongo/blob/master/db/extsort.cpp#L223) mmap failed


db/extsort.h
----
* 10052 [code](http://github.com/mongodb/mongo/blob/master/db/extsort.h#L114) not sorted


db/geo/2d.cpp
----
* 13022 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L96) can't have 2 geo field
* 13023 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L97) 2d has to be first in index
* 13024 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L106) no geo field specified
* 13026 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L300) geo values have to be numbers: 
* 13027 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L323) point not in interval of [ " << _min << ", " << _max << " )
* 13028 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L110) bits in geo index must be between 1 and 32
* 13042 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L2369) missing geo field (" + _geo + ") in : 
* 13046 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L2417) 'near' param missing/invalid", !cmdObj["near
* 13057 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L2341) $within has to take an object or array
* 13058 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L2359) unknown $with type: 
* 13059 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L2345) $center has to take an object or array
* 13060 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L1945) $center needs 2 fields (middle,max distance)
* 13061 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L1959) need a max distance >= 0 
* 13063 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L2081) $box needs 2 fields (bottomLeft,topRight)
* 13064 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L2090) need an area > 0 
* 13065 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L2350) $box has to take an object or array
* 13067 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L295) geo field is empty
* 13068 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L297) geo field only has 1 element
* 13460 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L1982) invalid $center query type: 
* 13461 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L1970) Spherical MaxDistance > PI. Are you sure you are using radians?
* 13462 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L1977) Spherical distance would require wrapping, which isn't implemented yet
* 13464 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L2315) invalid $near search type: 
* 13654 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L224) location object expected, location array not in correct format
* 13656 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L1950) the first field of $center object must be a location object
* 14029 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L2355) $polygon has to take an object or array
* 14030 [code](http://github.com/mongodb/mongo/blob/master/db/geo/2d.cpp#L2197) polygon must be defined by three points or more


db/geo/core.h
----
* 13047 [code](http://github.com/mongodb/mongo/blob/master/db/geo/core.h#L85) wrong type for geo index. if you're using a pre-release version, need to rebuild index
* 14808 [code](http://github.com/mongodb/mongo/blob/master/db/geo/core.h#L466) point " << p.toString() << " must be in earth-like bounds of long : [-180, 180), lat : [-90, 90] 


db/geo/haystack.cpp
----
* 13314 [code](http://github.com/mongodb/mongo/blob/master/db/geo/haystack.cpp#L89) can't have 2 geo fields
* 13315 [code](http://github.com/mongodb/mongo/blob/master/db/geo/haystack.cpp#L90) 2d has to be first in index
* 13316 [code](http://github.com/mongodb/mongo/blob/master/db/geo/haystack.cpp#L99) no geo field specified
* 13317 [code](http://github.com/mongodb/mongo/blob/master/db/geo/haystack.cpp#L100) no other fields specified
* 13318 [code](http://github.com/mongodb/mongo/blob/master/db/geo/haystack.cpp#L298) near needs to be an array
* 13319 [code](http://github.com/mongodb/mongo/blob/master/db/geo/haystack.cpp#L299) maxDistance needs a number
* 13320 [code](http://github.com/mongodb/mongo/blob/master/db/geo/haystack.cpp#L300) search needs to be an object
* 13321 [code](http://github.com/mongodb/mongo/blob/master/db/geo/haystack.cpp#L80) need bucketSize
* 13322 [code](http://github.com/mongodb/mongo/blob/master/db/geo/haystack.cpp#L106) not a number
* 13323 [code](http://github.com/mongodb/mongo/blob/master/db/geo/haystack.cpp#L141) latlng not an array
* 13326 [code](http://github.com/mongodb/mongo/blob/master/db/geo/haystack.cpp#L101) quadrant search can only have 1 other field for now


db/index.cpp
----
* 10096 [code](http://github.com/mongodb/mongo/blob/master/db/index.cpp#L228) invalid ns to index
* 10097 [code](http://github.com/mongodb/mongo/blob/master/db/index.cpp#L229) bad table to index name on add index attempt
* 10098 [code](http://github.com/mongodb/mongo/blob/master/db/index.cpp#L236) 
* 11001 [code](http://github.com/mongodb/mongo/blob/master/db/index.cpp#L60) E11001 duplicate key on update
* 12504 [code](http://github.com/mongodb/mongo/blob/master/db/index.cpp#L243) 
* 12505 [code](http://github.com/mongodb/mongo/blob/master/db/index.cpp#L273) 
* 12523 [code](http://github.com/mongodb/mongo/blob/master/db/index.cpp#L224) no index name specified
* 12524 [code](http://github.com/mongodb/mongo/blob/master/db/index.cpp#L233) index key pattern too large
* 12588 [code](http://github.com/mongodb/mongo/blob/master/db/index.cpp#L279) cannot add index with a background operation in progress
* 14803 [code](http://github.com/mongodb/mongo/blob/master/db/index.cpp#L316) this version of mongod cannot build new indexes of version number 


db/index.h
----
* 14802 [code](http://github.com/mongodb/mongo/blob/master/db/index.h#L149) index v field should be Integer type


db/indexkey.cpp
----
* 10088 [code](http://github.com/mongodb/mongo/blob/master/db/indexkey.cpp#L163) 
* 13007 [code](http://github.com/mongodb/mongo/blob/master/db/indexkey.cpp#L59) can only have 1 index plugin / bad index key pattern
* 13529 [code](http://github.com/mongodb/mongo/blob/master/db/indexkey.cpp#L76) sparse only works for single field keys


db/instance.cpp
----
* 10054 [code](http://github.com/mongodb/mongo/blob/master/db/instance.cpp#L462) not master
* 10055 [code](http://github.com/mongodb/mongo/blob/master/db/instance.cpp#L449) update object too large
* 10056 [code](http://github.com/mongodb/mongo/blob/master/db/instance.cpp#L490) not master
* 10058 [code](http://github.com/mongodb/mongo/blob/master/db/instance.cpp#L617) not master
* 10059 [code](http://github.com/mongodb/mongo/blob/master/db/instance.cpp#L574) object to insert too large
* 10309 [code](http://github.com/mongodb/mongo/blob/master/db/instance.cpp#L919) Unable to create/open lock file: " << name << ' ' << errnoWithDescription() << " Is a mongod instance already running?
* 10310 [code](http://github.com/mongodb/mongo/blob/master/db/instance.cpp#L924) Unable to acquire lock for lockfilepath: 
* 12596 [code](http://github.com/mongodb/mongo/blob/master/db/instance.cpp#L977) old lock file
* 13004 [code](http://github.com/mongodb/mongo/blob/master/db/instance.cpp#L394) sent negative cursors to kill: 
* 13073 [code](http://github.com/mongodb/mongo/blob/master/db/instance.cpp#L529) shutting down
* 13342 [code](http://github.com/mongodb/mongo/blob/master/db/instance.cpp#L995) Unable to truncate lock file
* 13455 [code](http://github.com/mongodb/mongo/blob/master/db/instance.cpp#L836) dbexit timed out getting lock
* 13511 [code](http://github.com/mongodb/mongo/blob/master/db/instance.cpp#L580) document to insert can't have $ fields
* 13597 [code](http://github.com/mongodb/mongo/blob/master/db/instance.cpp#L987) can't start without --journal enabled when journal/ files are present
* 13618 [code](http://github.com/mongodb/mongo/blob/master/db/instance.cpp#L1012) can't start without --journal enabled when journal/ files are present
* 13625 [code](http://github.com/mongodb/mongo/blob/master/db/instance.cpp#L991) Unable to truncate lock file
* 13627 [code](http://github.com/mongodb/mongo/blob/master/db/instance.cpp#L913) Unable to create/open lock file: " << name << ' ' << m << " Is a mongod instance already running?
* 13637 [code](http://github.com/mongodb/mongo/blob/master/db/instance.cpp#L720) count failed in DBDirectClient: 
* 13658 [code](http://github.com/mongodb/mongo/blob/master/db/instance.cpp#L393) bad kill cursors size: 
* 13659 [code](http://github.com/mongodb/mongo/blob/master/db/instance.cpp#L392) sent 0 cursors to kill


db/jsobj.cpp
----
* 10060 [code](http://github.com/mongodb/mongo/blob/master/db/jsobj.cpp#L678) woSortOrder needs a non-empty sortKey
* 10061 [code](http://github.com/mongodb/mongo/blob/master/db/jsobj.cpp#L1259) type not supported for appendMinElementForType
* 10311 [code](http://github.com/mongodb/mongo/blob/master/db/jsobj.cpp#L125) 
* 10312 [code](http://github.com/mongodb/mongo/blob/master/db/jsobj.cpp#L284) 
* 12579 [code](http://github.com/mongodb/mongo/blob/master/db/jsobj.cpp#L982) unhandled cases in BSONObj okForStorage


db/json.cpp
----
* 10338 [code](http://github.com/mongodb/mongo/blob/master/db/json.cpp#L230) Invalid use of reserved field name
* 10339 [code](http://github.com/mongodb/mongo/blob/master/db/json.cpp#L370) Badly formatted bindata
* 10340 [code](http://github.com/mongodb/mongo/blob/master/db/json.cpp#L584) Failure parsing JSON string near: 


db/lasterror.cpp
----
* 13649 [code](http://github.com/mongodb/mongo/blob/master/db/lasterror.cpp#L88) no operation yet


db/matcher.cpp
----
* 10066 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L328) $where occurs twice?
* 10067 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L329) $where query, but no script engine
* 10068 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L192) invalid operator: 
* 10069 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L278) BUG - can't operator for: 
* 10070 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L888) $where compile error
* 10071 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L903) 
* 10072 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L907) unknown error in invocation of $where function
* 10073 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L100) mod can't be 0
* 10341 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L82) scope has to be created first!
* 10342 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L1049) pcre not compiled with utf8 support
* 12517 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L107) $elemMatch needs an Object
* 13020 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L156) with $all, can't mix $elemMatch and others
* 13021 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L576) $all/$elemMatch needs to be applied to array
* 13029 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L269) can't use $not with $options, use BSON regex type instead
* 13030 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L378) $not cannot be empty
* 13031 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L388) invalid use of $not
* 13032 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L258) can't use $not with $regex, use BSON regex type instead
* 13086 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L285) $or/$nor must be a nonempty array
* 13087 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L289) $or/$nor match element must be an object
* 13089 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L330) no current client needed for $where
* 13090 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L284) nested $or/$nor not allowed
* 13276 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L217) $in needs an array
* 13277 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L228) $nin needs an array
* 13629 [code](http://github.com/mongodb/mongo/blob/master/db/matcher.cpp#L320) can't have undefined in a query expression


db/mongommf.cpp
----
* 13520 [code](http://github.com/mongodb/mongo/blob/master/db/mongommf.cpp#L260) MongoMMF only supports filenames in a certain format 
* 13636 [code](http://github.com/mongodb/mongo/blob/master/db/mongommf.cpp#L286) file " << filename() << " open/create failed in createPrivateMap (look in log for more information)


db/mongomutex.h
----
* 10293 [code](http://github.com/mongodb/mongo/blob/master/db/mongomutex.h#L235) internal error: locks are not upgradeable: 
* 12599 [code](http://github.com/mongodb/mongo/blob/master/db/mongomutex.h#L101) internal error: attempt to unlock when wasn't in a write lock


db/namespace-inl.h
----
* 10080 [code](http://github.com/mongodb/mongo/blob/master/db/namespace-inl.h#L35) ns name too long, max size is 128
* 10348 [code](http://github.com/mongodb/mongo/blob/master/db/namespace-inl.h#L45) $extra: ns name too long
* 10349 [code](http://github.com/mongodb/mongo/blob/master/db/namespace-inl.h#L103) E12000 idxNo fails
* 13283 [code](http://github.com/mongodb/mongo/blob/master/db/namespace-inl.h#L90) missing Extra
* 14045 [code](http://github.com/mongodb/mongo/blob/master/db/namespace-inl.h#L82) missing Extra


db/namespace.cpp
----
* 10079 [code](http://github.com/mongodb/mongo/blob/master/db/namespace.cpp#L174) bad .ns file length, cannot open database
* 10082 [code](http://github.com/mongodb/mongo/blob/master/db/namespace.cpp#L467) allocExtra: too many namespaces/collections
* 10343 [code](http://github.com/mongodb/mongo/blob/master/db/namespace.cpp#L181) bad lenForNewNsFiles
* 10346 [code](http://github.com/mongodb/mongo/blob/master/db/namespace.cpp#L530) not implemented
* 10350 [code](http://github.com/mongodb/mongo/blob/master/db/namespace.cpp#L462) allocExtra: base ns missing?
* 10351 [code](http://github.com/mongodb/mongo/blob/master/db/namespace.cpp#L463) allocExtra: extra already exists
* 14037 [code](http://github.com/mongodb/mongo/blob/master/db/namespace.cpp#L640) can't create user databases on a --configsvr instance


db/namespace.h
----
* 10081 [code](http://github.com/mongodb/mongo/blob/master/db/namespace.h#L567) too many namespaces/collections


db/nonce.cpp
----
* 10352 [code](http://github.com/mongodb/mongo/blob/master/db/nonce.cpp#L30) Security is a singleton class
* 10353 [code](http://github.com/mongodb/mongo/blob/master/db/nonce.cpp#L40) can't open dev/urandom
* 10354 [code](http://github.com/mongodb/mongo/blob/master/db/nonce.cpp#L49) md5 unit test fails
* 10355 [code](http://github.com/mongodb/mongo/blob/master/db/nonce.cpp#L67) devrandom failed


db/oplog.cpp
----
* 13044 [code](http://github.com/mongodb/mongo/blob/master/db/oplog.cpp#L500) no ts field in query
* 13257 [code](http://github.com/mongodb/mongo/blob/master/db/oplog.cpp#L338) 
* 13288 [code](http://github.com/mongodb/mongo/blob/master/db/oplog.cpp#L48) replSet error write op to db before replSet initialized", str::startsWith(ns, "local.
* 13312 [code](http://github.com/mongodb/mongo/blob/master/db/oplog.cpp#L135) replSet error : logOp() but not primary?
* 13347 [code](http://github.com/mongodb/mongo/blob/master/db/oplog.cpp#L171) local.oplog.rs missing. did you drop it? if so restart server
* 13389 [code](http://github.com/mongodb/mongo/blob/master/db/oplog.cpp#L67) local.oplog.rs missing. did you drop it? if so restart server
* 14038 [code](http://github.com/mongodb/mongo/blob/master/db/oplog.cpp#L453) invalid _findingStartMode


db/pdfile.cpp
----
* 10003 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L1034) failing update: objects in a capped ns cannot grow
* 10083 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L226) invalid size spec
* 10084 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L402) can't map file memory - mongo requires 64 bit build for larger datasets
* 10085 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L404) can't map file memory
* 10086 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L816) ns not found: 
* 10087 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L824) turn off profiling before dropping system.profile collection
* 10089 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L980) can't remove from a capped collection
* 10092 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L1204) too may dups on index build with dropDups=true
* 10093 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L1712) cannot insert into reserved $ collection
* 10094 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L1713) invalid ns: 
* 10095 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L1611) attempt to insert in reserved database name 'system'
* 10099 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L1752) _id cannot be an array
* 10356 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L314) invalid ns: 
* 10357 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L443) shutdown in progress
* 10358 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L444) bad new extent size
* 10359 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L445) header==0 on new extent: 32 bit mmap space exceeded?
* 10360 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L588) Extent::reset bad magic value
* 10361 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L796) can't create .$freelist
* 12502 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L826) can't drop system ns
* 12503 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L864) 
* 12582 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L1551) duplicate key insert for unique index of capped collection
* 12583 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L1828) unexpected index insertion failure on capped collection
* 12584 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L1345) cursor gone during bg index
* 12585 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L1325) cursor gone during bg index; dropDups
* 12586 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L89) cannot perform operation: a background operation is currently running for this database
* 12587 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L94) cannot perform operation: a background operation is currently running for this collection
* 13130 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L1359) can't start bg index b/c in recursive lock (db.eval?)
* 13143 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L1654) can't create index on system.indexes" , tabletoidxns.find( ".system.indexes
* 13440 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L354) 
* 13441 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L348) 
* 13596 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L1029) cannot change _id of a document old:" << objOld << " new:
* 14051 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L1619) system.user entry needs 'user' field to be a string" , t["user
* 14052 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L1620) system.user entry needs 'pwd' field to be a string" , t["pwd
* 14053 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L1621) system.user entry needs 'user' field to be non-empty" , t["user
* 14054 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.cpp#L1622) system.user entry needs 'pwd' field to be non-empty" , t["pwd


db/pdfile.h
----
* 13640 [code](http://github.com/mongodb/mongo/blob/master/db/pdfile.h#L348) DataFileHeader looks corrupt at file open filelength:" << filelength << " fileno:


db/projection.cpp
----
* 10053 [code](http://github.com/mongodb/mongo/blob/master/db/projection.cpp#L82) You cannot currently mix including and excluding fields. Contact us if this is an issue.
* 10371 [code](http://github.com/mongodb/mongo/blob/master/db/projection.cpp#L25) can only add to Projection once
* 13097 [code](http://github.com/mongodb/mongo/blob/master/db/projection.cpp#L64) Unsupported projection option: 
* 13098 [code](http://github.com/mongodb/mongo/blob/master/db/projection.cpp#L60) $slice only supports numbers and [skip, limit] arrays
* 13099 [code](http://github.com/mongodb/mongo/blob/master/db/projection.cpp#L50) $slice array wrong size
* 13100 [code](http://github.com/mongodb/mongo/blob/master/db/projection.cpp#L55) $slice limit must be positive


db/query.cpp
----
* 10100 [code](http://github.com/mongodb/mongo/blob/master/db/query.cpp#L138) cannot delete from collection with reserved $ in name
* 10101 [code](http://github.com/mongodb/mongo/blob/master/db/query.cpp#L145) can't remove from a capped collection
* 10110 [code](http://github.com/mongodb/mongo/blob/master/db/query.cpp#L1040) bad query object
* 12050 [code](http://github.com/mongodb/mongo/blob/master/db/query.cpp#L134) cannot delete from system namespace
* 13051 [code](http://github.com/mongodb/mongo/blob/master/db/query.cpp#L1053) tailable cursor requested on non capped collection
* 13052 [code](http://github.com/mongodb/mongo/blob/master/db/query.cpp#L1059) only {$natural:1} order allowed for tailable cursor
* 13340 [code](http://github.com/mongodb/mongo/blob/master/db/query.cpp#L75) cursor dropped during delete
* 13530 [code](http://github.com/mongodb/mongo/blob/master/db/query.cpp#L1018) bad or malformed command request?
* 13638 [code](http://github.com/mongodb/mongo/blob/master/db/query.cpp#L859) client cursor dropped during explain query yield


db/query.h
----
* 10102 [code](http://github.com/mongodb/mongo/blob/master/db/query.h#L130) bad order array
* 10103 [code](http://github.com/mongodb/mongo/blob/master/db/query.h#L131) bad order array [2]
* 10104 [code](http://github.com/mongodb/mongo/blob/master/db/query.h#L134) too many ordering elements
* 10105 [code](http://github.com/mongodb/mongo/blob/master/db/query.h#L213) bad skip value in query
* 12001 [code](http://github.com/mongodb/mongo/blob/master/db/query.h#L292) E12001 can't sort with $snapshot
* 12002 [code](http://github.com/mongodb/mongo/blob/master/db/query.h#L293) E12002 can't use hint with $snapshot
* 13513 [code](http://github.com/mongodb/mongo/blob/master/db/query.h#L262) sort must be an object or array


db/queryoptimizer.cpp
----
* 10111 [code](http://github.com/mongodb/mongo/blob/master/db/queryoptimizer.cpp#L44) table scans not allowed:
* 10112 [code](http://github.com/mongodb/mongo/blob/master/db/queryoptimizer.cpp#L349) bad hint
* 10113 [code](http://github.com/mongodb/mongo/blob/master/db/queryoptimizer.cpp#L361) bad hint
* 10363 [code](http://github.com/mongodb/mongo/blob/master/db/queryoptimizer.cpp#L213) newCursor() with start location not implemented for indexed plans
* 10364 [code](http://github.com/mongodb/mongo/blob/master/db/queryoptimizer.cpp#L234) newReverseCursor() not implemented for indexed plans
* 10365 [code](http://github.com/mongodb/mongo/blob/master/db/queryoptimizer.cpp#L327) 
* 10366 [code](http://github.com/mongodb/mongo/blob/master/db/queryoptimizer.cpp#L388) natural order cannot be specified with $min/$max
* 10367 [code](http://github.com/mongodb/mongo/blob/master/db/queryoptimizer.cpp#L399) 
* 10368 [code](http://github.com/mongodb/mongo/blob/master/db/queryoptimizer.cpp#L459) Unable to locate previously recorded index
* 10369 [code](http://github.com/mongodb/mongo/blob/master/db/queryoptimizer.cpp#L643) no plans
* 13038 [code](http://github.com/mongodb/mongo/blob/master/db/queryoptimizer.cpp#L435) can't find special index: " + _special + " for: 
* 13040 [code](http://github.com/mongodb/mongo/blob/master/db/queryoptimizer.cpp#L97) no type for special: 
* 13268 [code](http://github.com/mongodb/mongo/blob/master/db/queryoptimizer.cpp#L836) invalid $or spec
* 13292 [code](http://github.com/mongodb/mongo/blob/master/db/queryoptimizer.cpp#L336) hint eoo


db/queryoptimizer.h
----
* 13266 [code](http://github.com/mongodb/mongo/blob/master/db/queryoptimizer.h#L436) not implemented for $or query
* 13271 [code](http://github.com/mongodb/mongo/blob/master/db/queryoptimizer.h#L439) can't run more ops
* 13335 [code](http://github.com/mongodb/mongo/blob/master/db/queryoptimizer.h#L142) yield not supported
* 13336 [code](http://github.com/mongodb/mongo/blob/master/db/queryoptimizer.h#L144) yield not supported


db/queryoptimizercursor.cpp
----
* 14809 [code](http://github.com/mongodb/mongo/blob/master/db/queryoptimizercursor.cpp#L262) Invalid access for cursor that is not ok()


db/queryutil.cpp
----
* 10370 [code](http://github.com/mongodb/mongo/blob/master/db/queryutil.cpp#L323) $all requires array
* 12580 [code](http://github.com/mongodb/mongo/blob/master/db/queryutil.cpp#L160) invalid query
* 13033 [code](http://github.com/mongodb/mongo/blob/master/db/queryutil.cpp#L628) can't have 2 special fields
* 13034 [code](http://github.com/mongodb/mongo/blob/master/db/queryutil.cpp#L792) invalid use of $not
* 13041 [code](http://github.com/mongodb/mongo/blob/master/db/queryutil.cpp#L801) invalid use of $not
* 13050 [code](http://github.com/mongodb/mongo/blob/master/db/queryutil.cpp#L738) $all requires array
* 13262 [code](http://github.com/mongodb/mongo/blob/master/db/queryutil.cpp#L1356) $or requires nonempty array
* 13263 [code](http://github.com/mongodb/mongo/blob/master/db/queryutil.cpp#L1360) $or array must contain objects
* 13274 [code](http://github.com/mongodb/mongo/blob/master/db/queryutil.cpp#L1372) no or clause to pop
* 13291 [code](http://github.com/mongodb/mongo/blob/master/db/queryutil.cpp#L1362) $or may not contain 'special' query
* 13303 [code](http://github.com/mongodb/mongo/blob/master/db/queryutil.cpp#L998) combinatorial limit of $in partitioning of result set exceeded
* 13304 [code](http://github.com/mongodb/mongo/blob/master/db/queryutil.cpp#L1008) combinatorial limit of $in partitioning of result set exceeded
* 13385 [code](http://github.com/mongodb/mongo/blob/master/db/queryutil.cpp#L867) combinatorial limit of $in partitioning of result set exceeded
* 13454 [code](http://github.com/mongodb/mongo/blob/master/db/queryutil.cpp#L228) invalid regular expression operator
* 14048 [code](http://github.com/mongodb/mongo/blob/master/db/queryutil.cpp#L1085) FieldRangeSetPair invalid index specified
* 14049 [code](http://github.com/mongodb/mongo/blob/master/db/queryutil.cpp#L1089) FieldRangeSetPair invalid index specified


db/repl.cpp
----
* 10002 [code](http://github.com/mongodb/mongo/blob/master/db/repl.cpp#L388) local.sources collection corrupt?
* 10118 [code](http://github.com/mongodb/mongo/blob/master/db/repl.cpp#L256) 'host' field not set in sources collection object
* 10119 [code](http://github.com/mongodb/mongo/blob/master/db/repl.cpp#L257) only source='main' allowed for now with replication", sourceName() == "main
* 10120 [code](http://github.com/mongodb/mongo/blob/master/db/repl.cpp#L260) bad sources 'syncedTo' field value
* 10123 [code](http://github.com/mongodb/mongo/blob/master/db/repl.cpp#L992) replication error last applied optime at slave >= nextOpTime from master
* 10124 [code](http://github.com/mongodb/mongo/blob/master/db/repl.cpp#L1182) 
* 10384 [code](http://github.com/mongodb/mongo/blob/master/db/repl.cpp#L399) --only requires use of --source
* 10385 [code](http://github.com/mongodb/mongo/blob/master/db/repl.cpp#L455) Unable to get database list
* 10386 [code](http://github.com/mongodb/mongo/blob/master/db/repl.cpp#L764) non Date ts found: 
* 10389 [code](http://github.com/mongodb/mongo/blob/master/db/repl.cpp#L793) Unable to get database list
* 10390 [code](http://github.com/mongodb/mongo/blob/master/db/repl.cpp#L880) got $err reading remote oplog
* 10391 [code](http://github.com/mongodb/mongo/blob/master/db/repl.cpp#L885) repl: bad object read from remote oplog
* 10392 [code](http://github.com/mongodb/mongo/blob/master/db/repl.cpp#L1060) bad user object? [1]
* 10393 [code](http://github.com/mongodb/mongo/blob/master/db/repl.cpp#L1061) bad user object? [2]
* 13344 [code](http://github.com/mongodb/mongo/blob/master/db/repl.cpp#L876) trying to slave off of a non-master
* 14032 [code](http://github.com/mongodb/mongo/blob/master/db/repl.cpp#L559) Invalid 'ts' in remote log
* 14033 [code](http://github.com/mongodb/mongo/blob/master/db/repl.cpp#L565) Unable to get database list
* 14034 [code](http://github.com/mongodb/mongo/blob/master/db/repl.cpp#L607) Duplicate database names present after attempting to delete duplicates


db/repl/health.h
----
* 13112 [code](http://github.com/mongodb/mongo/blob/master/db/repl/health.h#L41) bad replset heartbeat option
* 13113 [code](http://github.com/mongodb/mongo/blob/master/db/repl/health.h#L42) bad replset heartbeat option


db/repl/rs.cpp
----
* 13093 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs.cpp#L257) bad --replSet config string format is: <setname>[/<seedhost1>,<seedhost2>,...]
* 13096 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs.cpp#L276) bad --replSet command line config string - dups?
* 13101 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs.cpp#L278) can't use localhost in replset host list
* 13114 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs.cpp#L274) bad --replSet seed hostname
* 13290 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs.cpp#L330) bad replSet oplog entry?
* 13302 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs.cpp#L424) replSet error self appears twice in the repl set configuration
* 13497 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs.cpp#L422) replSet error self not present in the configuration


db/repl/rs_config.cpp
----
* 13107 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L305) 
* 13108 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L315) bad replset config -- duplicate hosts in the config object?
* 13109 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L396) multiple rows in " << rsConfigNs << " not supported host: 
* 13115 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L229) bad " + rsConfigNs + " config: version
* 13117 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L322) bad " + rsConfigNs + " config
* 13122 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L326) bad repl set config?
* 13126 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L116) bad Member config
* 13131 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L252) replSet error parsing (or missing) 'members' field in config object
* 13132 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L203) 
* 13133 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L207) replSet bad config no members
* 13135 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L311) 
* 13260 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L371) 
* 13308 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L206) replSet bad config version #
* 13309 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L208) replSet bad config maximum number of members is 12
* 13393 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L321) can't use localhost in repl set member names except when using it for all members
* 13419 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L123) priorities must be between 0.0 and 100.0
* 13432 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L170) _id may not change for members
* 13433 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L187) can't find self in new replset config
* 13434 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L39) unexpected field '" << e.fieldName() << "'in object
* 13437 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L124) slaveDelay requires priority be zero
* 13438 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L125) bad slaveDelay value
* 13439 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L126) priority must be 0 when hidden=true
* 13476 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L174) buildIndexes may not change for members
* 13477 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L127) priority must be 0 when buildIndexes=false
* 13510 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L180) arbiterOnly may not change for members
* 13612 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L215) replSet bad config maximum number of voting members is 7
* 13613 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L216) replSet bad config no voting members
* 13645 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_config.cpp#L164) hosts cannot switch between localhost and hostname


db/repl/rs_initialsync.cpp
----
* 13404 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_initialsync.cpp#L41) 


db/repl/rs_initiate.cpp
----
* 13144 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_initiate.cpp#L127) 
* 13145 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_initiate.cpp#L92) set name does not match the set name host " + i->h.toString() + " expects
* 13256 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_initiate.cpp#L96) member " + i->h.toString() + " is already initiated
* 13259 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_initiate.cpp#L82) 
* 13278 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_initiate.cpp#L58) bad config: isSelf is true for multiple hosts: 
* 13279 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_initiate.cpp#L64) 
* 13311 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_initiate.cpp#L133) member " + i->h.toString() + " has data already, cannot initiate set.  All members except initiator must be empty.
* 13341 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_initiate.cpp#L101) member " + i->h.toString() + " has a config version >= to the new cfg version; cannot change config
* 13420 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_initiate.cpp#L51) initiation and reconfiguration of a replica set must be sent to a node that can become primary


db/repl/rs_rollback.cpp
----
* 13410 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_rollback.cpp#L344) replSet too much data to roll back
* 13423 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_rollback.cpp#L461) replSet error in rollback can't find 


db/repl/rs_sync.cpp
----
* 1000 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_sync.cpp#L299) replSet source for syncing doesn't seem to be await capable -- is it an older version of mongodb?
* 12000 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_sync.cpp#L385) rs slaveDelay differential too big check clocks and systems
* 13508 [code](http://github.com/mongodb/mongo/blob/master/db/repl/rs_sync.cpp#L87) no 'ts' in first op in oplog: 


db/replutil.h
----
* 10107 [code](http://github.com/mongodb/mongo/blob/master/db/replutil.h#L75) not master
* 13435 [code](http://github.com/mongodb/mongo/blob/master/db/replutil.h#L83) not master and slaveok=false
* 13436 [code](http://github.com/mongodb/mongo/blob/master/db/replutil.h#L84) not master or secondary, can't read


db/restapi.cpp
----
* 13085 [code](http://github.com/mongodb/mongo/blob/master/db/restapi.cpp#L151) query failed for dbwebserver


db/scanandorder.h
----
* 10128 [code](http://github.com/mongodb/mongo/blob/master/db/scanandorder.h#L123) too much data for sort() with no index.  add an index or specify a smaller limit
* 10129 [code](http://github.com/mongodb/mongo/blob/master/db/scanandorder.h#L147) too much data for sort() with no index


db/update.cpp
----
* 10131 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L120) $push can only be applied to an array
* 10132 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L191) $pushAll can only be applied to an array
* 10133 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L192) $pushAll has to be passed an array
* 10134 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L216) $pull/$pullAll can only be applied to an array
* 10135 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L249) $pop can only be applied to an array
* 10136 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L285) $bit needs an array
* 10137 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L286) $bit can only be applied to numbers
* 10138 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L287) $bit can't use a double
* 10139 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L295) $bit field must be number
* 10140 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L406) Cannot apply $inc modifier to non-number
* 10141 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L428) Cannot apply $push/$pushAll modifier to non-array
* 10142 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L434) Cannot apply $pull/$pullAll modifier to non-array
* 10143 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L455) Cannot apply $pop modifier to non-array
* 10145 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L667) LEFT_SUBFIELD only supports Object: " << field << " not: 
* 10147 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L801) Invalid modifier specified: 
* 10148 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L813) Mod on _id not allowed", strcmp( fieldName, "_id
* 10149 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L814) Invalid mod field name, may not end in a period
* 10150 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L815) Field name duplication not allowed with modifiers
* 10151 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L816) have conflicting mods in update
* 10152 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L817) Modifier $inc allowed for numbers only
* 10153 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L818) Modifier $pushAll/pullAll allowed for arrays only
* 10154 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L894) Modifiers and non-modifiers cannot be mixed
* 10155 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L1277) cannot update reserved $ collection
* 10156 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L1280) cannot update system collection: " << ns << " q: " << patternOrig << " u: 
* 10157 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L1125) multi-update requires all modified objects to have an _id
* 10158 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L1234) multi update only works with $ operators
* 10159 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L1264) multi update only works with $ operators
* 10399 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L712) ModSet::createNewFromMods - RIGHT_SUBFIELD should be impossible
* 10400 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L715) unhandled case
* 12522 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L953) $ operator made object too large
* 12591 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L461) Cannot apply $addToSet modifier to non-array
* 12592 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L137) $addToSet can only be applied to an array
* 13339 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L917) cursor dropped during update
* 13478 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L601) can't apply mod in place - shouldn't have gotten here
* 13479 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L825) invalid mod field name, target may not be empty
* 13480 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L826) invalid mod field name, source may not begin or end in period
* 13481 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L827) invalid mod field name, target may not begin or end in period
* 13482 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L828) $rename affecting _id not allowed
* 13483 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L829) $rename affecting _id not allowed
* 13484 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L830) field name duplication not allowed with $rename target
* 13485 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L831) conflicting mods not allowed with $rename target
* 13486 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L832) $rename target may not be a parent of source
* 13487 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L833) $rename source may not be dynamic array", strstr( fieldName , ".$
* 13488 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L834) $rename target may not be dynamic array", strstr( target , ".$
* 13489 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L378) $rename source field invalid
* 13490 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L389) $rename target field invalid
* 13494 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L821) $rename target must be a string
* 13495 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L823) $rename source must differ from target
* 13496 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L824) invalid mod field name, source may not be empty
* 9016 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L312) unknown bit mod:
* 9017 [code](http://github.com/mongodb/mongo/blob/master/db/update.cpp#L337) 


db/update.h
----
* 10161 [code](http://github.com/mongodb/mongo/blob/master/db/update.h#L344) Invalid modifier specified 
* 12527 [code](http://github.com/mongodb/mongo/blob/master/db/update.h#L210) not okForStorage
* 13492 [code](http://github.com/mongodb/mongo/blob/master/db/update.h#L235) mod must be RENAME_TO type
* 9015 [code](http://github.com/mongodb/mongo/blob/master/db/update.h#L588) 


dbtests/framework.cpp
----
* 10162 [code](http://github.com/mongodb/mongo/blob/master/dbtests/framework.cpp#L395) already have suite with that name


dbtests/jsobjtests.cpp
----
* 10403 [code](http://github.com/mongodb/mongo/blob/master/dbtests/jsobjtests.cpp#L1234) 
* 10404 [code](http://github.com/mongodb/mongo/blob/master/dbtests/jsobjtests.cpp#L1235) 
* 10405 [code](http://github.com/mongodb/mongo/blob/master/dbtests/jsobjtests.cpp#L1236) 
* 10406 [code](http://github.com/mongodb/mongo/blob/master/dbtests/jsobjtests.cpp#L1237) 
* 10407 [code](http://github.com/mongodb/mongo/blob/master/dbtests/jsobjtests.cpp#L1238) 
* 12528 [code](http://github.com/mongodb/mongo/blob/master/dbtests/jsobjtests.cpp#L1664) should be ok for storage:
* 12529 [code](http://github.com/mongodb/mongo/blob/master/dbtests/jsobjtests.cpp#L1671) should NOT be ok for storage:


dbtests/queryoptimizertests.cpp
----
* 10408 [code](http://github.com/mongodb/mongo/blob/master/dbtests/queryoptimizertests.cpp#L558) throw
* 10409 [code](http://github.com/mongodb/mongo/blob/master/dbtests/queryoptimizertests.cpp#L597) throw
* 10410 [code](http://github.com/mongodb/mongo/blob/master/dbtests/queryoptimizertests.cpp#L716) throw
* 10411 [code](http://github.com/mongodb/mongo/blob/master/dbtests/queryoptimizertests.cpp#L729) throw


s/balance.cpp
----
* 13258 [code](http://github.com/mongodb/mongo/blob/master/s/balance.cpp#L292) oids broken after resetting!


s/chunk.cpp
----
* 10163 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L124) can only handle numbers here - which i think is correct
* 10165 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L261) can't split as shard doesn't have a manager
* 10167 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L299) can't move shard to its current location!
* 10169 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L417) datasize failed!" , conn->runCommand( "admin
* 10170 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L64) Chunk needs a ns
* 10171 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L67) Chunk needs a server
* 10172 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L69) Chunk needs a min
* 10173 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L70) Chunk needs a max
* 10174 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L768) config servers not all up
* 10412 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L395) 
* 13003 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L264) can't split a chunk with only one distinct value
* 13141 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L655) Chunk map pointed to incorrect chunk
* 13282 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L526) Couldn't load a valid config for " + _ns + " after 3 attempts. Please try again.
* 13327 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L65) Chunk ns must match server ns
* 13331 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L766) collection's metadata is undergoing changes. Please try again.
* 13332 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L262) need a split key to split chunk
* 13333 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L263) can't split a chunk in that many parts
* 13345 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L186) 
* 13346 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L816) can't pre-split already splitted collection
* 13405 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L734) min must have shard key
* 13406 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L735) max must have shard key
* 13501 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L680) use geoNear command rather than $near query
* 13502 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L687) unrecognized special query type: 
* 13503 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L157) 
* 13507 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L711) invalid chunk config minObj: 
* 13592 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L618) 
* 14022 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L763) Error locking distributed lock for chunk drop.
* 8070 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L659) couldn't find a chunk which should be impossible: 
* 8071 [code](http://github.com/mongodb/mongo/blob/master/s/chunk.cpp#L806) cleaning up after drop failed: 


s/client.cpp
----
* 13134 [code](http://github.com/mongodb/mongo/blob/master/s/client.cpp#L63) 


s/commands_public.cpp
----
* 10418 [code](http://github.com/mongodb/mongo/blob/master/s/commands_public.cpp#L264) how could chunk manager be null!
* 10420 [code](http://github.com/mongodb/mongo/blob/master/s/commands_public.cpp#L719) how could chunk manager be null!
* 12594 [code](http://github.com/mongodb/mongo/blob/master/s/commands_public.cpp#L487) how could chunk manager be null!
* 13002 [code](http://github.com/mongodb/mongo/blob/master/s/commands_public.cpp#L609) how could chunk manager be null!
* 13091 [code](http://github.com/mongodb/mongo/blob/master/s/commands_public.cpp#L784) how could chunk manager be null!
* 13092 [code](http://github.com/mongodb/mongo/blob/master/s/commands_public.cpp#L785) GridFS chunks collection can only be sharded on files_id", cm->getShardKey().key() == BSON("files_id
* 13137 [code](http://github.com/mongodb/mongo/blob/master/s/commands_public.cpp#L321) Source and destination collections must be on same shard
* 13138 [code](http://github.com/mongodb/mongo/blob/master/s/commands_public.cpp#L315) You can't rename a sharded collection
* 13139 [code](http://github.com/mongodb/mongo/blob/master/s/commands_public.cpp#L316) You can't rename to a sharded collection
* 13140 [code](http://github.com/mongodb/mongo/blob/master/s/commands_public.cpp#L314) Don't recognize source or target DB
* 13343 [code](http://github.com/mongodb/mongo/blob/master/s/commands_public.cpp#L612) query for sharded findAndModify must have shardkey
* 13398 [code](http://github.com/mongodb/mongo/blob/master/s/commands_public.cpp#L335) cant copy to sharded DB
* 13399 [code](http://github.com/mongodb/mongo/blob/master/s/commands_public.cpp#L343) need a fromdb argument
* 13400 [code](http://github.com/mongodb/mongo/blob/master/s/commands_public.cpp#L346) don't know where source DB is
* 13401 [code](http://github.com/mongodb/mongo/blob/master/s/commands_public.cpp#L347) cant copy from sharded DB
* 13402 [code](http://github.com/mongodb/mongo/blob/master/s/commands_public.cpp#L332) need a todb argument
* 13407 [code](http://github.com/mongodb/mongo/blob/master/s/commands_public.cpp#L641) how could chunk manager be null!
* 13408 [code](http://github.com/mongodb/mongo/blob/master/s/commands_public.cpp#L647) keyPattern must equal shard key
* 13500 [code](http://github.com/mongodb/mongo/blob/master/s/commands_public.cpp#L815) how could chunk manager be null!
* 13512 [code](http://github.com/mongodb/mongo/blob/master/s/commands_public.cpp#L267) drop collection attempted on non-sharded collection


s/config.cpp
----
* 10176 [code](http://github.com/mongodb/mongo/blob/master/s/config.cpp#L386) shard state missing for 
* 10178 [code](http://github.com/mongodb/mongo/blob/master/s/config.cpp#L111) no primary!
* 10181 [code](http://github.com/mongodb/mongo/blob/master/s/config.cpp#L188) not sharded:
* 10184 [code](http://github.com/mongodb/mongo/blob/master/s/config.cpp#L389) _dropShardedCollections too many collections - bailing
* 10187 [code](http://github.com/mongodb/mongo/blob/master/s/config.cpp#L422) need configdbs
* 10189 [code](http://github.com/mongodb/mongo/blob/master/s/config.cpp#L579) should only have 1 thing in config.version
* 13396 [code](http://github.com/mongodb/mongo/blob/master/s/config.cpp#L263) DBConfig save failed: 
* 13449 [code](http://github.com/mongodb/mongo/blob/master/s/config.cpp#L144) collections already sharded
* 13473 [code](http://github.com/mongodb/mongo/blob/master/s/config.cpp#L85) failed to save collection (" + ns + "): 
* 13509 [code](http://github.com/mongodb/mongo/blob/master/s/config.cpp#L216) can't migrate from 1.5.x release to the current one; need to upgrade to 1.6.x first
* 13648 [code](http://github.com/mongodb/mongo/blob/master/s/config.cpp#L128) can't shard collection because not all config servers are up
* 8042 [code](http://github.com/mongodb/mongo/blob/master/s/config.cpp#L127) db doesn't have sharding enabled
* 8043 [code](http://github.com/mongodb/mongo/blob/master/s/config.cpp#L135) collection already sharded


s/config.h
----
* 10190 [code](http://github.com/mongodb/mongo/blob/master/s/config.h#L197) ConfigServer not setup
* 8041 [code](http://github.com/mongodb/mongo/blob/master/s/config.h#L143) no primary shard configured for db: 


s/cursors.cpp
----
* 10191 [code](http://github.com/mongodb/mongo/blob/master/s/cursors.cpp#L75) cursor already done
* 13286 [code](http://github.com/mongodb/mongo/blob/master/s/cursors.cpp#L216) sent 0 cursors to kill
* 13287 [code](http://github.com/mongodb/mongo/blob/master/s/cursors.cpp#L217) too many cursors to kill


s/d_chunk_manager.cpp
----
* 13539 [code](http://github.com/mongodb/mongo/blob/master/s/d_chunk_manager.cpp#L48)  does not exist
* 13540 [code](http://github.com/mongodb/mongo/blob/master/s/d_chunk_manager.cpp#L49)  collection config entry corrupted" , collectionDoc["dropped
* 13541 [code](http://github.com/mongodb/mongo/blob/master/s/d_chunk_manager.cpp#L50)  dropped. Re-shard collection first." , !collectionDoc["dropped
* 13542 [code](http://github.com/mongodb/mongo/blob/master/s/d_chunk_manager.cpp#L76) collection doesn't have a key: 
* 13585 [code](http://github.com/mongodb/mongo/blob/master/s/d_chunk_manager.cpp#L226) version " << version.toString() << " not greater than 
* 13586 [code](http://github.com/mongodb/mongo/blob/master/s/d_chunk_manager.cpp#L195) couldn't find chunk " << min << "->
* 13587 [code](http://github.com/mongodb/mongo/blob/master/s/d_chunk_manager.cpp#L203) 
* 13588 [code](http://github.com/mongodb/mongo/blob/master/s/d_chunk_manager.cpp#L260) 
* 13590 [code](http://github.com/mongodb/mongo/blob/master/s/d_chunk_manager.cpp#L217) setting version to " << version << " on removing last chunk
* 13591 [code](http://github.com/mongodb/mongo/blob/master/s/d_chunk_manager.cpp#L246) version can't be set to zero
* 14039 [code](http://github.com/mongodb/mongo/blob/master/s/d_chunk_manager.cpp#L285) version " << version.toString() << " not greater than 
* 14040 [code](http://github.com/mongodb/mongo/blob/master/s/d_chunk_manager.cpp#L292) can split " << min << " -> " << max << " on 


s/d_logic.cpp
----
* 10422 [code](http://github.com/mongodb/mongo/blob/master/s/d_logic.cpp#L96) write with bad shard config and no server id!


s/d_split.cpp
----
* 13593 [code](http://github.com/mongodb/mongo/blob/master/s/d_split.cpp#L771) 


s/d_state.cpp
----
* 13298 [code](http://github.com/mongodb/mongo/blob/master/s/d_state.cpp#L78) 
* 13299 [code](http://github.com/mongodb/mongo/blob/master/s/d_state.cpp#L100) 
* 13647 [code](http://github.com/mongodb/mongo/blob/master/s/d_state.cpp#L512) context should be empty here, is: 


s/grid.cpp
----
* 10185 [code](http://github.com/mongodb/mongo/blob/master/s/grid.cpp#L93) can't find a shard to put new db on
* 10186 [code](http://github.com/mongodb/mongo/blob/master/s/grid.cpp#L107) removeDB expects db name
* 10421 [code](http://github.com/mongodb/mongo/blob/master/s/grid.cpp#L440) getoptime failed" , conn->simpleCommand( "admin" , &result , "getoptime


s/request.cpp
----
* 10192 [code](http://github.com/mongodb/mongo/blob/master/s/request.cpp#L65) db config reload failed!
* 10193 [code](http://github.com/mongodb/mongo/blob/master/s/request.cpp#L69) no shard info for: 
* 10194 [code](http://github.com/mongodb/mongo/blob/master/s/request.cpp#L88) can't call primaryShard on a sharded collection!
* 10195 [code](http://github.com/mongodb/mongo/blob/master/s/request.cpp#L123) too many attempts to update config, failing
* 13644 [code](http://github.com/mongodb/mongo/blob/master/s/request.cpp#L61) can't use 'local' database through mongos" , ! str::startsWith( getns() , "local.
* 8060 [code](http://github.com/mongodb/mongo/blob/master/s/request.cpp#L84) can't call primaryShard on a sharded collection


s/server.cpp
----
* 10197 [code](http://github.com/mongodb/mongo/blob/master/s/server.cpp#L187) createDirectClient not implemented for sharding yet


s/shard.cpp
----
* 13128 [code](http://github.com/mongodb/mongo/blob/master/s/shard.cpp#L118) can't find shard for: 
* 13129 [code](http://github.com/mongodb/mongo/blob/master/s/shard.cpp#L110) can't find shard for: 
* 13136 [code](http://github.com/mongodb/mongo/blob/master/s/shard.cpp#L306) 
* 13632 [code](http://github.com/mongodb/mongo/blob/master/s/shard.cpp#L39) couldn't get updated shard list from config server
* 14807 [code](http://github.com/mongodb/mongo/blob/master/s/shard.cpp#L254) no set name for shard: " << _name << " 


s/shard_version.cpp
----
* 10428 [code](http://github.com/mongodb/mongo/blob/master/s/shard_version.cpp#L132) need_authoritative set but in authoritative mode already
* 10429 [code](http://github.com/mongodb/mongo/blob/master/s/shard_version.cpp#L155) 


s/shardconnection.cpp
----
* 13409 [code](http://github.com/mongodb/mongo/blob/master/s/shardconnection.cpp#L234) can't parse ns from: 


s/shardkey.cpp
----
* 10198 [code](http://github.com/mongodb/mongo/blob/master/s/shardkey.cpp#L46) left object doesn't have full shard key
* 10199 [code](http://github.com/mongodb/mongo/blob/master/s/shardkey.cpp#L48) right object doesn't have full shard key


s/shardkey.h
----
* 13334 [code](http://github.com/mongodb/mongo/blob/master/s/shardkey.h#L106) Shard Key must be less than 512 bytes


s/strategy.cpp
----
* 10200 [code](http://github.com/mongodb/mongo/blob/master/s/strategy.cpp#L56) mongos: error calling db


s/strategy_shard.cpp
----
* 10201 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_shard.cpp#L205) invalid update
* 10203 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_shard.cpp#L305) bad delete message
* 12376 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_shard.cpp#L261) 
* 13123 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_shard.cpp#L248) 
* 13465 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_shard.cpp#L220) shard key in upsert query must be an exact match
* 13505 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_shard.cpp#L307) $atomic not supported sharded" , pattern["$atomic
* 13506 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_shard.cpp#L204) $atomic not supported sharded" , query["$atomic
* 14804 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_shard.cpp#L184) collection no longer sharded
* 14805 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_shard.cpp#L331) collection no longer sharded
* 14806 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_shard.cpp#L293) collection no longer sharded
* 8010 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_shard.cpp#L41) something is wrong, shouldn't see a command here
* 8011 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_shard.cpp#L149) tried to insert object without shard key
* 8012 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_shard.cpp#L214) can't upsert something without shard key
* 8013 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_shard.cpp#L229) can't do non-multi update with query that doesn't have the shard key
* 8014 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_shard.cpp#L256) 
* 8015 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_shard.cpp#L336) can only delete with a non-shard key pattern if can delete as many as we find
* 8016 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_shard.cpp#L364) can't do this write op on sharded collection


s/strategy_single.cpp
----
* 10204 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_single.cpp#L106) dbgrid: getmore: error calling db
* 10205 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_single.cpp#L124) can't use unique indexes with sharding  ns:
* 13390 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_single.cpp#L86) unrecognized command: 
* 8050 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_single.cpp#L145) can't update system.indexes
* 8051 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_single.cpp#L149) can't delete indexes on sharded collection yet
* 8052 [code](http://github.com/mongodb/mongo/blob/master/s/strategy_single.cpp#L153) handleIndexWrite invalid write op


s/util.h
----
* 13657 [code](http://github.com/mongodb/mongo/blob/master/s/util.h#L108) unknown type for ShardChunkVersion: 


s/writeback_listener.cpp
----
* 10427 [code](http://github.com/mongodb/mongo/blob/master/s/writeback_listener.cpp#L161) invalid writeback message
* 13403 [code](http://github.com/mongodb/mongo/blob/master/s/writeback_listener.cpp#L110) didn't get writeback for: " << oid << " after: " << t.millis() << " ms
* 13641 [code](http://github.com/mongodb/mongo/blob/master/s/writeback_listener.cpp#L69) can't parse host [" << conn.getServerAddress() << "]
* 14041 [code](http://github.com/mongodb/mongo/blob/master/s/writeback_listener.cpp#L100) got writeback waitfor for older id 


scripting/engine.cpp
----
* 10206 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine.cpp#L83) 
* 10207 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine.cpp#L90) compile failed
* 10208 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine.cpp#L176) need to have locallyConnected already
* 10209 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine.cpp#L197) name has to be a string: 
* 10210 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine.cpp#L198) value has to be set
* 10430 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine.cpp#L168) invalid object id: not hex
* 10448 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine.cpp#L159) invalid object id: length


scripting/engine.h
----
* 13474 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine.h#L194) no _getInterruptSpecCallback
* 9004 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine.h#L93) invoke failed: 
* 9005 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine.h#L101) invoke failed: 


scripting/engine_java.h
----
* 10211 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_java.h#L197) only readOnly setObject supported in java


scripting/engine_spidermonkey.cpp
----
* 10212 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L83) holder magic value is wrong
* 10213 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L223) non ascii character detected
* 10214 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L247) not a number
* 10215 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L323) not an object
* 10216 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L332) not a function
* 10217 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L389) can't append field.  name:" + name + " type: 
* 10218 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L699) not done: toval
* 10219 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L726) object passed to getPropery is null
* 10220 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L817) don't know what to do with this op
* 10221 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L1143) JS_NewRuntime failed
* 10222 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L1151) assert not being executed
* 10223 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L1226) deleted SMScope twice?
* 10224 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L1284) already local connected
* 10225 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L1294) already setup for external db
* 10226 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L1296) connected to different db
* 10227 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L1365) unknown type
* 10228 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L1521)  exec failed: 
* 10229 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L1720) need a scope
* 10431 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L1202) JS_NewContext failed
* 10432 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L1209) JS_NewObject failed for global
* 10433 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L1211) js init failed
* 13072 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L36) JS_NewObject failed: 
* 13076 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L153) recursive toObject
* 13498 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L216) 
* 13615 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L40) JS allocation failed, either memory leak or using too much memory
* 9006 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_spidermonkey.cpp#L46) invalid utf8


scripting/engine_v8.cpp
----
* 10230 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_v8.cpp#L462) can't handle external yet
* 10231 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_v8.cpp#L507) not an object
* 10232 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_v8.cpp#L569) not a func
* 10233 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_v8.cpp#L679) 
* 10234 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_v8.cpp#L706) 
* 12509 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_v8.cpp#L470) don't know what this is: 
* 12510 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_v8.cpp#L771) externalSetup already called, can't call externalSetup
* 12511 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_v8.cpp#L775) localConnect called with a different name previously
* 12512 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_v8.cpp#L797) localConnect already called, can't call externalSetup
* 13475 [code](http://github.com/mongodb/mongo/blob/master/scripting/engine_v8.cpp#L689) 


scripting/sm_db.cpp
----
* 10235 [code](http://github.com/mongodb/mongo/blob/master/scripting/sm_db.cpp#L78) no cursor!
* 10236 [code](http://github.com/mongodb/mongo/blob/master/scripting/sm_db.cpp#L83) no args to internal_cursor_constructor
* 10237 [code](http://github.com/mongodb/mongo/blob/master/scripting/sm_db.cpp#L156) mongo_constructor not implemented yet
* 10239 [code](http://github.com/mongodb/mongo/blob/master/scripting/sm_db.cpp#L214) no connection!
* 10245 [code](http://github.com/mongodb/mongo/blob/master/scripting/sm_db.cpp#L284) no connection!
* 10248 [code](http://github.com/mongodb/mongo/blob/master/scripting/sm_db.cpp#L312) no connection!
* 10251 [code](http://github.com/mongodb/mongo/blob/master/scripting/sm_db.cpp#L347) no connection!


scripting/utils.cpp
----
* 10261 [code](http://github.com/mongodb/mongo/blob/master/scripting/utils.cpp#L29) js md5 needs a string


shell/shell_utils.cpp
----
* 10257 [code](http://github.com/mongodb/mongo/blob/master/shell/shell_utils.cpp#L124) need to specify 1 argument to listFiles
* 10258 [code](http://github.com/mongodb/mongo/blob/master/shell/shell_utils.cpp#L105) processinfo not supported
* 12513 [code](http://github.com/mongodb/mongo/blob/master/shell/shell_utils.cpp#L934) connect failed", scope.exec( _dbConnect , "(connect)
* 12514 [code](http://github.com/mongodb/mongo/blob/master/shell/shell_utils.cpp#L937) login failed", scope.exec( _dbAuth , "(auth)
* 12518 [code](http://github.com/mongodb/mongo/blob/master/shell/shell_utils.cpp#L855) srand requires a single numeric argument
* 12519 [code](http://github.com/mongodb/mongo/blob/master/shell/shell_utils.cpp#L862) rand accepts no arguments
* 12581 [code](http://github.com/mongodb/mongo/blob/master/shell/shell_utils.cpp#L133) 
* 12597 [code](http://github.com/mongodb/mongo/blob/master/shell/shell_utils.cpp#L201) need to specify 1 argument
* 13006 [code](http://github.com/mongodb/mongo/blob/master/shell/shell_utils.cpp#L873) isWindows accepts no arguments
* 13301 [code](http://github.com/mongodb/mongo/blob/master/shell/shell_utils.cpp#L221) cat() : file to big to load as a variable
* 13411 [code](http://github.com/mongodb/mongo/blob/master/shell/shell_utils.cpp#L882) getHostName accepts no arguments
* 13619 [code](http://github.com/mongodb/mongo/blob/master/shell/shell_utils.cpp#L272) fuzzFile takes 2 arguments
* 13620 [code](http://github.com/mongodb/mongo/blob/master/shell/shell_utils.cpp#L275) couldn't open file to fuzz
* 13621 [code](http://github.com/mongodb/mongo/blob/master/shell/shell_utils.cpp#L628) no known mongo program on port
* 14042 [code](http://github.com/mongodb/mongo/blob/master/shell/shell_utils.cpp#L536) 


tools/dump.cpp
----
* 10262 [code](http://github.com/mongodb/mongo/blob/master/tools/dump.cpp#L104) couldn't open file
* 14035 [code](http://github.com/mongodb/mongo/blob/master/tools/dump.cpp#L58) couldn't write to file


tools/import.cpp
----
* 10263 [code](http://github.com/mongodb/mongo/blob/master/tools/import.cpp#L282) unknown error reading file
* 13289 [code](http://github.com/mongodb/mongo/blob/master/tools/import.cpp#L60) Invalid UTF8 character detected
* 13295 [code](http://github.com/mongodb/mongo/blob/master/tools/import.cpp#L273) JSONArray file too large


tools/sniffer.cpp
----
* 10266 [code](http://github.com/mongodb/mongo/blob/master/tools/sniffer.cpp#L472) can't use --source twice
* 10267 [code](http://github.com/mongodb/mongo/blob/master/tools/sniffer.cpp#L473) source needs more args


tools/tool.cpp
----
* 10264 [code](http://github.com/mongodb/mongo/blob/master/tools/tool.cpp#L428) invalid object size: 
* 10265 [code](http://github.com/mongodb/mongo/blob/master/tools/tool.cpp#L464) counts don't match
* 9997 [code](http://github.com/mongodb/mongo/blob/master/tools/tool.cpp#L368) auth failed: 
* 9998 [code](http://github.com/mongodb/mongo/blob/master/tools/tool.cpp#L349) you need to specify fields
* 9999 [code](http://github.com/mongodb/mongo/blob/master/tools/tool.cpp#L328) file: " + fn ) + " doesn't exist


util/alignedbuilder.cpp
----
* 13524 [code](http://github.com/mongodb/mongo/blob/master/util/alignedbuilder.cpp#L82) out of memory AlignedBuilder
* 13584 [code](http://github.com/mongodb/mongo/blob/master/util/alignedbuilder.cpp#L27) out of memory AlignedBuilder


util/assert_util.h
----
* 10437 [code](http://github.com/mongodb/mongo/blob/master/util/assert_util.h#L233) unknown boost failed
* 123 [code](http://github.com/mongodb/mongo/blob/master/util/assert_util.h#L74) blah
* 13294 [code](http://github.com/mongodb/mongo/blob/master/util/assert_util.h#L231) 
* 14043 [code](http://github.com/mongodb/mongo/blob/master/util/assert_util.h#L242) 
* 14044 [code](http://github.com/mongodb/mongo/blob/master/util/assert_util.h#L244) unknown boost failed 


util/background.cpp
----
* 13643 [code](http://github.com/mongodb/mongo/blob/master/util/background.cpp#L52) backgroundjob already started: 


util/base64.cpp
----
* 10270 [code](http://github.com/mongodb/mongo/blob/master/util/base64.cpp#L79) invalid base64


util/concurrency/list.h
----
* 14050 [code](http://github.com/mongodb/mongo/blob/master/util/concurrency/list.h#L82) List1: item to orphan not in list


util/file.h
----
* 10438 [code](http://github.com/mongodb/mongo/blob/master/util/file.h#L115) ReadFile error - truncated file?


util/file_allocator.cpp
----
* 10439 [code](http://github.com/mongodb/mongo/blob/master/util/file_allocator.cpp#L255) 
* 10440 [code](http://github.com/mongodb/mongo/blob/master/util/file_allocator.cpp#L157) 
* 10441 [code](http://github.com/mongodb/mongo/blob/master/util/file_allocator.cpp#L161) Unable to allocate new file of size 
* 10442 [code](http://github.com/mongodb/mongo/blob/master/util/file_allocator.cpp#L163) Unable to allocate new file of size 
* 10443 [code](http://github.com/mongodb/mongo/blob/master/util/file_allocator.cpp#L178) FileAllocator: file write failed
* 13653 [code](http://github.com/mongodb/mongo/blob/master/util/file_allocator.cpp#L274) 


util/hostandport.h
----
* 13095 [code](http://github.com/mongodb/mongo/blob/master/util/hostandport.h#L172) HostAndPort: bad port #
* 13110 [code](http://github.com/mongodb/mongo/blob/master/util/hostandport.h#L168) HostAndPort: bad config string


util/httpclient.cpp
----
* 10271 [code](http://github.com/mongodb/mongo/blob/master/util/httpclient.cpp#L39) invalid url" , url.find( "http://


util/log.cpp
----
* 10268 [code](http://github.com/mongodb/mongo/blob/master/util/log.cpp#L47) LoggingManager already started
* 14036 [code](http://github.com/mongodb/mongo/blob/master/util/log.cpp#L68) couldn't write to log file


util/logfile.cpp
----
* 13514 [code](http://github.com/mongodb/mongo/blob/master/util/logfile.cpp#L176) error appending to file on fsync 
* 13515 [code](http://github.com/mongodb/mongo/blob/master/util/logfile.cpp#L166) error appending to file 
* 13516 [code](http://github.com/mongodb/mongo/blob/master/util/logfile.cpp#L141) couldn't open file " << name << " for writing 
* 13517 [code](http://github.com/mongodb/mongo/blob/master/util/logfile.cpp#L93) error appending to file 
* 13518 [code](http://github.com/mongodb/mongo/blob/master/util/logfile.cpp#L70) couldn't open file " << name << " for writing 
* 13519 [code](http://github.com/mongodb/mongo/blob/master/util/logfile.cpp#L91) error 87 appending to file - misaligned direct write?


util/message.h
----
* 13273 [code](http://github.com/mongodb/mongo/blob/master/util/message.h#L320) single data buffer expected


util/message_server_asio.cpp
----
* 10273 [code](http://github.com/mongodb/mongo/blob/master/util/message_server_asio.cpp#L110) _cur not empty! pipelining requests not supported
* 10274 [code](http://github.com/mongodb/mongo/blob/master/util/message_server_asio.cpp#L171) pipelining requests doesn't work yet


util/message_server_port.cpp
----
* 10275 [code](http://github.com/mongodb/mongo/blob/master/util/message_server_port.cpp#L101) multiple PortMessageServer not supported


util/mmap.cpp
----
* 13468 [code](http://github.com/mongodb/mongo/blob/master/util/mmap.cpp#L34) can't create file already exists 
* 13617 [code](http://github.com/mongodb/mongo/blob/master/util/mmap.cpp#L172) MongoFile : multiple opens of same filename


util/mmap_posix.cpp
----
* 10446 [code](http://github.com/mongodb/mongo/blob/master/util/mmap_posix.cpp#L80) mmap: can't map area of size 0 file: 
* 10447 [code](http://github.com/mongodb/mongo/blob/master/util/mmap_posix.cpp#L90) map file alloc failed, wanted: " << length << " filelen: 


util/mmap_win.cpp
----
* 13056 [code](http://github.com/mongodb/mongo/blob/master/util/mmap_win.cpp#L190) Async flushing not supported on windows


util/paths.h
----
* 13600 [code](http://github.com/mongodb/mongo/blob/master/util/paths.h#L57) 
* 13646 [code](http://github.com/mongodb/mongo/blob/master/util/paths.h#L86) stat() failed for file: " << path << " 
* 13650 [code](http://github.com/mongodb/mongo/blob/master/util/paths.h#L107) Couldn't open directory '" << dir.string() << "' for flushing: 
* 13651 [code](http://github.com/mongodb/mongo/blob/master/util/paths.h#L111) Couldn't fsync directory '" << dir.string() << "': 
* 13652 [code](http://github.com/mongodb/mongo/blob/master/util/paths.h#L101) Couldn't find parent dir for file: 


util/processinfo_linux2.cpp
----
* 13538 [code](http://github.com/mongodb/mongo/blob/master/util/processinfo_linux2.cpp#L45) 


util/sock.cpp
----
* 13079 [code](http://github.com/mongodb/mongo/blob/master/util/sock.cpp#L54) path to unix socket too long
* 13080 [code](http://github.com/mongodb/mongo/blob/master/util/sock.cpp#L52) no unix socket support on windows


util/sock.h
----
* 13082 [code](http://github.com/mongodb/mongo/blob/master/util/sock.h#L178) 


util/text.h
----
* 13305 [code](http://github.com/mongodb/mongo/blob/master/util/text.h#L131) could not convert string to long long
* 13306 [code](http://github.com/mongodb/mongo/blob/master/util/text.h#L140) could not convert string to long long
* 13307 [code](http://github.com/mongodb/mongo/blob/master/util/text.h#L126) cannot convert empty string to long long
* 13310 [code](http://github.com/mongodb/mongo/blob/master/util/text.h#L144) could not convert string to long long

