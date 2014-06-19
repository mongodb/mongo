MongoDB Error Codes
==========




src/mongo/bson/bson-inl.h
----
* 10065 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L190) 
* 10313 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L535) Insufficient bytes to calculate element size
* 10314 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L539) Insufficient bytes to calculate element size
* 10315 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L544) Insufficient bytes to calculate element size
* 10316 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L549) Insufficient bytes to calculate element size
* 10317 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L553) Insufficient bytes to calculate element size
* 10318 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L559) Invalid regex string
* 10319 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L569) Invalid regex options string
* 10320 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L643) 
* 10327 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L482) Object does not end with EOO
* 10328 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L484) Invalid element size
* 10329 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L485) Element too large
* 10330 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L487) Element extends past end of object
* 10331 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L491) EOO Before end of object
* 10334 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L231) 
* 13655 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L577) 
* 16150 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson-inl.h#L664) 


src/mongo/bson/bson_db.h
----
* 10062 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bson_db.h#L78) not code


src/mongo/bson/bsonelement.h
----
* 10063 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonelement.h#L427) not a dbref
* 10064 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonelement.h#L432) not a dbref
* 10333 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonelement.h#L457) Invalid field name
* 13111 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonelement.h#L505) 
* 13118 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonelement.h#L510) unexpected or missing type value in BSON object
* 16177 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonelement.h#L282) not codeWScope
* 16178 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonelement.h#L289) not codeWScope


src/mongo/bson/bsonobjbuilder.h
----
* 10335 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonobjbuilder.h#L587) builder does not own memory
* 10336 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonobjbuilder.h#L654) No subobject started
* 13048 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonobjbuilder.h#L865) 
* 15891 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonobjbuilder.h#L875) can't backfill array to larger than 1,500,000 elements
* 16234 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonobjbuilder.h#L418) Invalid call to appendNull in BSONObj Builder.


src/mongo/bson/bsonobjiterator.h
----
* 16446 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/bsonobjiterator.h#L86) BSONElement has bad size


src/mongo/bson/ordering.h
----
* 13103 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/ordering.h#L79) too many compound keys


src/mongo/bson/util/builder.h
----
* 10000 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/util/builder.h#L119) out of memory BufBuilder
* 13548 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/util/builder.h#L232) 
* 15912 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/util/builder.h#L94) out of memory StackAllocator::Realloc
* 15913 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/util/builder.h#L144) out of memory BufBuilder::reset
* 16070 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/bson/util/builder.h#L236) out of memory BufBuilder::grow_reallocate


src/mongo/client/clientAndShell.cpp
----
* 10256 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/clientAndShell.cpp#L72) no createDirectClient in clientOnly


src/mongo/client/connpool.cpp
----
* 13071 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/connpool.cpp#L265) invalid hostname [" + host + "]
* 13328 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/connpool.cpp#L245) : connect failed " + url.toString() + " : 
* 17382 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/connpool.cpp#L200) Can't use connection pool during shutdown


src/mongo/client/connpool.h
----
* 11004 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/connpool.h#L323) connection was returned to the pool already
* 11005 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/connpool.h#L329) connection was returned to the pool already
* 13102 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/connpool.h#L335) connection was returned to the pool already


src/mongo/client/dbclient.cpp
----
* 10005 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L850) listdatabases failed" , runCommand( "admin" , BSON( "listDatabases
* 10006 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L851) listDatabases.databases not array" , info["databases
* 10007 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L1275) dropIndex failed
* 10008 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L1282) 
* 10276 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L907) DBClientBase::findN: transport error: " << getServerAddress() << " ns: " << ns << " query: 
* 10278 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L1437) dbclient error communicating with server: 
* 10337 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L1383) object not valid
* 11010 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L471) count fails:
* 13386 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L1127) socket error for mapping query
* 13421 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L162) trying to connect to invalid ConnectionString
* 16090 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L1095) socket error for mapping query
* 16335 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L149) custom connection to 
* 17232 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient.cpp#L586) You cannot specify both 'db' and 'userSource'. Please use only 'db'.


src/mongo/client/dbclient_rs.cpp
----
* 13639 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L316) can't connect to new replica set master [
* 16340 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L171) No replica set monitor active and no cached seed 
* 16369 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L372) No good nodes available for set: 
* 16370 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L548) 
* 16379 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L604) 
* 16380 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L788) 
* 16381 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L100) $readPreference should be an object
* 16382 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L104) mode not specified for read preference
* 16383 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L125) Unknown read preference mode: 
* 16384 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L135) Only empty tags are allowed with primary read preference
* 16385 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L130) tags for read preference should be an array
* 16532 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclient_rs.cpp#L706) Failed to connect to 


src/mongo/client/dbclientcursor.cpp
----
* 13127 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.cpp#L208) getMore: cursor didn't exist on server, possible restart or timeout?
* 13422 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.cpp#L262) DBClientCursor next() called but more() is false
* 14821 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.cpp#L328) No client or lazy client specified, cannot store multi-host connection.
* 15875 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.cpp#L92) DBClientCursor::initLazy called on a client that doesn't support lazy
* 16465 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.cpp#L188) recv failed while exhausting cursor


src/mongo/client/dbclientcursor.h
----
* 13106 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.h#L95) 
* 13348 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.h#L259) connection died
* 13383 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientcursor.h#L276) BatchIterator empty


src/mongo/client/dbclientinterface.h
----
* 10011 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientinterface.h#L782) no collection name
* 9000 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/dbclientinterface.h#L1198) 


src/mongo/client/gridfs.cpp
----
* 10012 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L112) file doesn't exist" , fileName == "-
* 10013 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L119) error opening file
* 10014 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L238) chunk is empty!
* 10015 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L270) doesn't exists
* 13296 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L82) invalid chunk size is specified
* 13325 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L264) couldn't open file: 
* 16428 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L157) 
* 9008 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/gridfs.cpp#L164) filemd5 failed


src/mongo/client/parallel.cpp
----
* 10019 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L1524) no more elements
* 13431 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L360) 
* 14812 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L1612) Error running command on server: 
* 14813 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L1613) Command returned nothing
* 15986 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L685) too many retries in total
* 15987 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L812) could not fully initialize cursor on shard 
* 15988 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L951) error querying server
* 15989 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L649) database not found for parallel cursor request
* 17306 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/parallel.cpp#L387) 


src/mongo/client/replica_set_monitor.cpp
----
* 10009 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/replica_set_monitor.cpp#L296) ReplicaSetMonitor no master found for set: 
* 13610 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/replica_set_monitor.cpp#L418) ConfigChangeHook already specified
* 13642 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/replica_set_monitor.cpp#L875) Replica set seed list can't be empty
* 16337 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/replica_set_monitor.cpp#L970) Unknown read preference
* 16358 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/replica_set_monitor.cpp#L929) Tags should be a BSON object


src/mongo/client/sasl_client_session.cpp
----
* 16807 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/sasl_client_session.cpp#L222) 


src/mongo/client/syncclusterconnection.cpp
----
* 10022 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L353) SyncClusterConnection::getMore not supported yet
* 13053 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L533) help failed: " << info , _commandOnActive( "admin" , BSON( name << "1" << "help
* 13054 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L291) write $cmd not supported in SyncClusterConnection::query for:
* 13105 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L219) 
* 13119 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L360) 
* 13120 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L423) SyncClusterConnection::update upsert query needs _id" , query.obj["_id
* 13397 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L509) SyncClusterConnection::say prepare failed: 
* 15848 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L238) sync cluster of sync clusters?
* 16743 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L385) 
* 16744 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L393) 
* 8001 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L156) SyncClusterConnection write op failed: 
* 8002 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L349) all servers down/unreachable when querying: 
* 8003 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L366) SyncClusterConnection::insert prepare failed: 
* 8004 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L67) SyncClusterConnection needs 3 servers
* 8005 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L429) SyncClusterConnection::update prepare failed: 
* 8006 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L483) SyncClusterConnection::call can only be used directly for dbQuery
* 8007 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L487) SyncClusterConnection::call can't handle $cmd" , strstr( d.getns(), "$cmd
* 8008 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L503) all servers down/unreachable: 
* 8020 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/client/syncclusterconnection.cpp#L411) SyncClusterConnection::remove prepare failed: 


src/mongo/db/auth/authorization_manager.cpp
----
* 16914 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authorization_manager.cpp#L535) 
* 17003 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authorization_manager.cpp#L536) 
* 17008 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authorization_manager.cpp#L537) 
* 17190 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authorization_manager.cpp#L173) 
* 17191 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authorization_manager.cpp#L197) 
* 17192 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authorization_manager.cpp#L234) 
* 17222 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authorization_manager.cpp#L188) 
* 17223 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authorization_manager.cpp#L225) 
* 17231 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authorization_manager.cpp#L226) 
* 17265 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authorization_manager.cpp#L256) 
* 17266 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authorization_manager.cpp#L681) 


src/mongo/db/auth/authorization_manager_global.cpp
----
* 16841 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authorization_manager_global.cpp#L81) 
* 16842 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authorization_manager_global.cpp#L92) 
* 16843 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authorization_manager_global.cpp#L86) 


src/mongo/db/auth/authorization_session.cpp
----
* 17067 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authorization_session.cpp#L446) 
* 17068 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authorization_session.cpp#L453) 


src/mongo/db/auth/authz_documents_update_guard.cpp
----
* 17126 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_documents_update_guard.cpp#L46) 
* 17127 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_documents_update_guard.cpp#L52) 


src/mongo/db/auth/authz_manager_external_state_local.cpp
----
* 17153 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_local.cpp#L105) 
* 17154 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_local.cpp#L107) 
* 17155 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_local.cpp#L115) 
* 17156 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_local.cpp#L126) 
* 17157 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_local.cpp#L128) 
* 17158 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_local.cpp#L196) 
* 17159 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_local.cpp#L195) 
* 17160 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_local.cpp#L198) 
* 17161 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_local.cpp#L206) 
* 17162 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_local.cpp#L226) 
* 17163 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_local.cpp#L228) 
* 17164 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_local.cpp#L233) 
* 17165 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_local.cpp#L235) 
* 17166 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_local.cpp#L240) 
* 17167 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_local.cpp#L269) 
* 17183 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_local.cpp#L389) 
* 17267 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_local.cpp#L230) 
* 17323 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_local.cpp#L259) 


src/mongo/db/auth/authz_manager_external_state_mock.cpp
----
* 17175 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_mock.cpp#L49) 
* 17176 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_mock.cpp#L50) 
* 17177 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_mock.cpp#L57) 
* 17178 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_mock.cpp#L68) 
* 17179 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_manager_external_state_mock.cpp#L70) 


src/mongo/db/auth/authz_session_external_state_d.cpp
----
* 17506 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/authz_session_external_state_d.cpp#L50) 


src/mongo/db/auth/mechanism_scram.cpp
----
* 17450 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/mechanism_scram.cpp#L62) invalid salt length provided
* 17494 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/mechanism_scram.cpp#L71) 
* 17495 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/mechanism_scram.cpp#L83) 
* 17498 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/mechanism_scram.cpp#L120) 
* 17499 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/mechanism_scram.cpp#L129) 
* 17500 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/mechanism_scram.cpp#L133) 


src/mongo/db/auth/role_graph.cpp
----
* 16825 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/role_graph.cpp#L79) 
* 16826 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/role_graph.cpp#L85) 
* 16827 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/role_graph.cpp#L241) 
* 17168 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/role_graph.cpp#L408) 
* 17169 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/role_graph.cpp#L413) 
* 17170 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/role_graph.cpp#L417) 
* 17171 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/role_graph.cpp#L418) 
* 17172 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/role_graph.cpp#L420) 
* 17173 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/role_graph.cpp#L270) 
* 17277 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/role_graph.cpp#L460) 


src/mongo/db/auth/role_graph_builtin_roles.cpp
----
* 17145 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/role_graph_builtin_roles.cpp#L773) 


src/mongo/db/auth/user_document_parser.cpp
----
* 17501 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/user_document_parser.cpp#L355) Invalid or missing SCRAM iteration count
* 17502 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/user_document_parser.cpp#L360) Missing SCRAM salt
* 17503 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/user_document_parser.cpp#L365) Missing SCRAM serverKey
* 17504 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/auth/user_document_parser.cpp#L370) Missing SCRAM storedKey


src/mongo/db/background.cpp
----
* 17431 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/background.cpp#L100) 


src/mongo/db/catalog/collection.cpp
----
* 10089 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/collection.cpp#L267) cannot remove from a capped collection
* 17445 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/collection.cpp#L445) index build in progress


src/mongo/db/catalog/collection_cursor_cache.cpp
----
* 12051 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/collection_cursor_cache.cpp#L409) 
* 16089 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/collection_cursor_cache.cpp#L484) 
* 17359 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/collection_cursor_cache.cpp#L142) 
* 17360 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/collection_cursor_cache.cpp#L446) 


src/mongo/db/catalog/database.cpp
----
* 10028 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/database.cpp#L119) 
* 14037 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/database.cpp#L464) can't create user databases on a --configsvr instance
* 17316 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/database.cpp#L475) cannot create a blank collection
* 17320 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/database.cpp#L59) 
* 17381 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/database.cpp#L469) fully qualified namespace " << ns << " is too long 
* 17399 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/database.cpp#L457) collection already exists


src/mongo/db/catalog/database_holder.cpp
----
* 15927 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/database_holder.cpp#L91) can't open database in a read lock. if db was just closed, consider retrying the query. might otherwise indicate an internal error
* 17507 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/database_holder.cpp#L70) Can't take a write lock while out of disk space


src/mongo/db/catalog/database_holder.h
----
* 13074 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/database_holder.h#L83) db name can't be empty
* 13075 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/database_holder.h#L86) db name can't be empty
* 13280 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/database_holder.h#L77) invalid db name: 


src/mongo/db/catalog/index_catalog.cpp
----
* 14803 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_catalog.cpp#L1122) this version of mongod cannot build new indexes of version number 
* 17197 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_catalog.cpp#L171) Invalid index type '" << pluginName << "' 
* 17198 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_catalog.cpp#L149) 
* 17204 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_catalog.cpp#L429) 
* 17206 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_catalog.cpp#L451) 
* 17207 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_catalog.cpp#L454) 
* 17324 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_catalog.cpp#L697) 
* 17325 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_catalog.cpp#L698) 
* 17326 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_catalog.cpp#L699) 
* 17327 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_catalog.cpp#L711) 
* 17328 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_catalog.cpp#L712) 
* 17330 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_catalog.cpp#L461) 
* 17331 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_catalog.cpp#L463) 
* 17334 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_catalog.cpp#L962) cannot find index entry
* 17336 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_catalog.cpp#L700) 
* 17337 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_catalog.cpp#L713) 
* 17340 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_catalog.cpp#L104) 
* 17345 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_catalog.cpp#L413) 
* 17348 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_catalog.cpp#L647) 
* 17357 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_catalog.cpp#L972) cannot find index entry


src/mongo/db/catalog/index_create.cpp
----
* 13130 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_create.cpp#L220) 
* 17342 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_create.cpp#L227) 
* 17343 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_create.cpp#L207) 
* 17398 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/index_create.cpp#L261) 


src/mongo/db/client.cpp
----
* 15928 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/client.cpp#L219) can't open a database from a nested read lock 


src/mongo/db/client_basic.cpp
----
* 16477 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/client_basic.cpp#L68) 
* 16481 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/client_basic.cpp#L61) 


src/mongo/db/cloner.cpp
----
* 10024 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L79) bad ns field for index during dbcopy
* 10025 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L81) bad ns field for index during dbcopy [2]
* 10289 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L352) useReplAuth is not written to replication log
* 10290 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L437) 
* 17321 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/cloner.cpp#L117) 


src/mongo/db/commands.cpp
----
* 17005 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands.cpp#L69) 


src/mongo/db/commands.h
----
* 16940 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands.h#L182) 


src/mongo/db/commands/collection_to_capped.cpp
----
* 16708 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/collection_to_capped.cpp#L139) bad 'toCollection' value


src/mongo/db/commands/copydb.cpp
----
* 13008 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/copydb.cpp#L160) must call copydbgetnonce first


src/mongo/db/commands/distinct.cpp
----
* 17216 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/distinct.cpp#L121) Can't get runner for query 
* 17217 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/distinct.cpp#L146) distinct too big, 16mb cap
* 18510 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/distinct.cpp#L77) 
* 18511 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/distinct.cpp#L84) 


src/mongo/db/commands/find_and_modify.cpp
----
* 12515 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/find_and_modify.cpp#L372) can't remove and update", cmdObj["update
* 12516 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/find_and_modify.cpp#L404) must specify remove or update
* 13329 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/find_and_modify.cpp#L343) upsert mode requires update field
* 13330 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/find_and_modify.cpp#L344) upsert mode requires query field
* 17383 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/find_and_modify.cpp#L146) Could not canonicalize 
* 17384 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/find_and_modify.cpp#L150) Could not get runner for query 


src/mongo/db/commands/find_and_modify_common.cpp
----
* 17137 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/find_and_modify_common.cpp#L66) Invalid target namespace 


src/mongo/db/commands/geonear.cpp
----
* 17296 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/geonear.cpp#L164) distanceMultiplier must be a number
* 17297 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/geonear.cpp#L166) distanceMultiplier must be non-negative
* 17298 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/geonear.cpp#L128) minDistance doesn't work on 2d index
* 17299 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/geonear.cpp#L123) maxDistance must be a number",cmdObj["maxDistance
* 17300 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/geonear.cpp#L129) minDistance must be a number",cmdObj["minDistance
* 17301 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/geonear.cpp#L110) 2dsphere index must have spherical: true
* 17302 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/geonear.cpp#L153) limit must be >=0
* 17303 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/geonear.cpp#L151) limit must be number
* 17304 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/geonear.cpp#L104) 'near' field must be point


src/mongo/db/commands/group.cpp
----
* 10041 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/group.cpp#L82) invoke failed in $keyf: 
* 10042 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/group.cpp#L84) return of $key has to be an object
* 17203 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/group.cpp#L170) group() can't handle more than 20000 unique keys
* 17211 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/group.cpp#L72) ns has to be set", p["ns
* 17212 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/group.cpp#L145) Can't canonicalize query 
* 17213 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/group.cpp#L151) Can't get runner for query 
* 17214 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/group.cpp#L177) 


src/mongo/db/commands/isself.cpp
----
* 13469 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/isself.cpp#L87) getifaddrs failure: 
* 13470 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/isself.cpp#L102) getnameinfo() failed: 
* 13472 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/isself.cpp#L151) getnameinfo() failed: 


src/mongo/db/commands/mr.cpp
----
* 10074 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L181) need values
* 10075 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L222) reduce -> multiple not supported yet
* 10076 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L556) rename failed: 
* 10077 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L1151) fast_emit takes 2 args
* 13069 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L1152) an emit can't be more than half max bson size
* 13070 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L200) value too large to reduce
* 13598 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L81) couldn't compile code for: 
* 13602 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L259) outType is no longer a valid option" , cmdObj["outType
* 13604 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L473) too much data for in memory map/reduce
* 13608 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L304) query has to be blank or an Object
* 13609 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L311) sort has to be blank or an Object
* 13630 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L633) attempted to insert into nonexistent
* 13631 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L659) attempted to insert into nonexistent
* 15921 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L451) splitVector failed: 
* 16054 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L265) 
* 16149 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L1219) cannot run map reduce without the js engine
* 16717 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L759) error initializing JavaScript reduceAll function
* 16718 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L776) error initializing JavaScript reduce/emit function
* 16719 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L796) error creating JavaScript reduce/finalize function
* 16720 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L814) error initializing JavaScript functions
* 17238 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L1305) Can't canonicalize query 
* 17239 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L1311) Can't get runner for query 
* 17305 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L360) createIndex failed for mr incLong ns: 
* 9014 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr.cpp#L99) map invoke failed: 


src/mongo/db/commands/mr_common.cpp
----
* 13522 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr_common.cpp#L75) 
* 13606 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr_common.cpp#L93) 'out' has to be a string or an object
* 15895 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr_common.cpp#L86) 
* 17142 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr_common.cpp#L112) 
* 17143 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/mr_common.cpp#L130) Invalid target namespace 


src/mongo/db/commands/pipeline_command.cpp
----
* 16954 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/pipeline_command.cpp#L164) cursor field must be missing or an object
* 16955 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/pipeline_command.cpp#L170) cursor object can't contain fields other than batchSize
* 16956 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/pipeline_command.cpp#L174) cursor.batchSize must be a number
* 16957 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/pipeline_command.cpp#L178) Cursor batchSize must not be negative
* 17391 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/pipeline_command.cpp#L238) 


src/mongo/db/commands/server_status_internal.cpp
----
* 16461 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/server_status_internal.cpp#L60) 


src/mongo/db/commands/test_commands.cpp
----
* 13049 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/test_commands.cpp#L60) godinsert must specify a collection
* 13416 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/test_commands.cpp#L137) captrunc must specify a collection
* 13417 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/test_commands.cpp#L144) captrunc collection not found or empty
* 13418 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/test_commands.cpp#L153) captrunc invalid n
* 13428 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/test_commands.cpp#L185) emptycapped must specify a collection
* 13429 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/test_commands.cpp#L191) emptycapped no such collection


src/mongo/db/commands/write_commands/write_commands_common.cpp
----
* 17251 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/commands/write_commands/write_commands_common.cpp#L87) 


src/mongo/db/curop.h
----
* 12601 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/curop.h#L268) CurOp not marked done yet


src/mongo/db/d_concurrency.cpp
----
* 16098 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L420) 
* 16099 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L597) 
* 16100 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L605) 
* 16103 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L92) 
* 16106 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L412) 
* 16114 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L103) 
* 16116 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L232) 
* 16117 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L233) 
* 16118 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L236) 
* 16119 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L247) 
* 16120 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L248) 
* 16121 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L255) 
* 16122 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L257) 
* 16123 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L258) 
* 16125 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L262) 
* 16126 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L264) 
* 16127 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L270) 
* 16128 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L272) 
* 16129 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L276) 
* 16130 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L278) 
* 16131 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L380) 
* 16132 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L385) 
* 16133 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L399) 
* 16134 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L439) 
* 16135 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L624) 
* 16171 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L185) 
* 16186 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L461) can't get a DBWrite while having a read lock
* 16187 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L631) 
* 16188 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L645) 
* 16189 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L651) 
* 16252 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L406) 
* 16253 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L454) 
* 16254 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L481) 
* 16255 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/d_concurrency.cpp#L591) 


src/mongo/db/db.cpp
----
* 10296 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/db.cpp#L636) 
* 12590 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/db.cpp#L641) 
* 18506 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/db.cpp#L358) 


src/mongo/db/dbcommands.cpp
----
* 10040 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L830) chunks out of order
* 13455 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L118) dbexit timed out getting lock
* 14832 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L598) 
* 15888 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L580) 
* 16247 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L784) md5 state not correct size
* 17240 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L800) Can't canonicalize query 
* 17241 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L806) Can't get runner for query 
* 17317 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands.cpp#L1317) impersonation unexpectedly active


src/mongo/db/dbcommands_generic.cpp
----
* 10038 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbcommands_generic.cpp#L342) forced error


src/mongo/db/dbeval.cpp
----
* 10046 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbeval.cpp#L52) eval needs Code


src/mongo/db/dbhelpers.cpp
----
* 13430 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbhelpers.cpp#L170) no _id index
* 17244 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbhelpers.cpp#L109) Could not canonicalize 
* 17245 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbhelpers.cpp#L114) Could not get runner for query 


src/mongo/db/dbmessage.h
----
* 10304 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbmessage.h#L208) 
* 10307 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbmessage.h#L214) 
* 13066 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbmessage.h#L206) Message contains no documents


src/mongo/db/dbwebserver.cpp
----
* 13453 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbwebserver.cpp#L218) server not started with --jsonp
* 17051 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbwebserver.cpp#L139) 
* 17090 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/dbwebserver.cpp#L142) 


src/mongo/db/exec/filter.h
----
* 16920 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/filter.h#L85) trying to match on unknown field: 
* 17409 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/filter.h#L130) 


src/mongo/db/exec/stagedebug_cmd.cpp
----
* 16890 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L204) Can't find index: 
* 16910 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L190) Unknown fieldname 
* 16911 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L138) Couldn't parse plan from 
* 16921 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L217) Nodes argument must be provided to AND
* 16922 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L226) node of AND isn't an obj?: 
* 16923 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L230) Can't parse sub-node of AND: 
* 16924 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L242) Nodes argument must be provided to AND
* 16925 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L252) node of AND isn't an obj?: 
* 16926 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L256) Can't parse sub-node of AND: 
* 16927 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L237) AND requires more than one child
* 16928 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L263) AND requires more than one child
* 16929 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L288) Node argument must be provided to fetch
* 16930 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L299) Node argument must be provided to limit
* 16931 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L301) Num argument must be provided to limit
* 16932 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L312) Node argument must be provided to skip
* 16933 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L314) Num argument must be provided to skip
* 16934 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L268) Nodes argument must be provided to AND
* 16935 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L270) Dedup argument must be provided to OR
* 16936 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L279) Can't parse sub-node of OR: 
* 16937 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L297) Limit stage doesn't have a filter (put it on the child)
* 16938 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L310) Skip stage doesn't have a filter (put it on the child)
* 16963 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L327) Direction argument must be specified and be a number
* 16969 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L341) Node argument must be provided to sort
* 16970 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L343) Pattern argument must be provided to sort
* 16971 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L352) Nodes argument must be provided to sort
* 16972 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L354) Pattern argument must be provided to sort
* 16973 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L367) node of mergeSort isn't an obj?: 
* 16974 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L371) Can't parse sub-node of mergeSort: 
* 17194 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L383) Expected exactly one text index
* 17446 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/exec/stagedebug_cmd.cpp#L124) Couldn't find the collection 


src/mongo/db/fts/fts_index_format.cpp
----
* 16675 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_index_format.cpp#L101) cannot have a multi-key as a prefix to a text index
* 16732 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_index_format.cpp#L121) 
* 16733 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_index_format.cpp#L158) 


src/mongo/db/fts/fts_spec.cpp
----
* 16674 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L385) score for word too high
* 16730 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L467) 
* 16739 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L69) found invalid spec for text index
* 17136 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L431) 
* 17261 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L149) 
* 17262 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L153) 
* 17263 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L416) 
* 17264 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L420) 
* 17271 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L291) 
* 17272 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L298) expecting _ftsx:1
* 17273 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L311) 
* 17274 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L340) 
* 17283 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L368) 
* 17284 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L378) text index option 'weights' must be an object
* 17288 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L338) expected _ftsx after _fts
* 17289 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L267) 
* 17291 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L396) 
* 17292 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L399) 
* 17293 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L463) 
* 17294 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L391) 
* 17364 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L78) 
* 17367 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L72) 
* 17389 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec.cpp#L354) 


src/mongo/db/fts/fts_spec_legacy.cpp
----
* 17365 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec_legacy.cpp#L254) score for word too high
* 17366 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/fts/fts_spec_legacy.cpp#L296) 


src/mongo/db/geo/geoparser.cpp
----
* 17125 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/geoparser.cpp#L102) 


src/mongo/db/geo/geoquery.cpp
----
* 16672 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/geoquery.cpp#L238) $within not supported with provided geometry: 
* 16681 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/geoquery.cpp#L122) $near requires geojson point, given 
* 16885 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/geoquery.cpp#L115) $near requires a point, given 
* 16893 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/geoquery.cpp#L59) $minDistance must be a number
* 16894 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/geoquery.cpp#L61) $minDistance must be non-negative
* 16895 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/geoquery.cpp#L63) $maxDistance must be a number
* 16896 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/geoquery.cpp#L65) $maxDistance must be non-negative
* 16897 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/geoquery.cpp#L127) $minDistance must be a number
* 16898 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/geoquery.cpp#L129) $minDistance must be non-negative
* 16899 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/geoquery.cpp#L131) $maxDistance must be a number
* 16900 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/geoquery.cpp#L133) $maxDistance must be non-negative
* 17444 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/geoquery.cpp#L73) 


src/mongo/db/geo/hash.cpp
----
* 13026 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/hash.cpp#L611) 
* 13027 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/hash.cpp#L617) 
* 13047 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/hash.cpp#L158) wrong type for geo index. if you're using a pre-release version,
* 13067 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/hash.cpp#L599) 
* 13068 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/hash.cpp#L605) 
* 16433 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/hash.cpp#L630) 
* 16457 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/hash.cpp#L139) initFromString passed a too-long string
* 16458 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/hash.cpp#L140) initFromString passed an odd length string 


src/mongo/db/geo/haystack.cpp
----
* 13318 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/haystack.cpp#L103) near needs to be an array
* 13319 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/haystack.cpp#L104) maxDistance needs a number
* 13320 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/haystack.cpp#L105) search needs to be an object


src/mongo/db/geo/shapes.cpp
----
* 14808 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/geo/shapes.cpp#L511) point 


src/mongo/db/global_environment_experiment.cpp
----
* 17508 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/global_environment_experiment.cpp#L42) 
* 17509 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/global_environment_experiment.cpp#L47) 


src/mongo/db/global_optime.cpp
----
* 17449 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/global_optime.cpp#L70) 


src/mongo/db/index/btree_access_method.cpp
----
* 16745 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/btree_access_method.cpp#L64) Invalid index version for key generation.


src/mongo/db/index/btree_key_generator.cpp
----
* 16746 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/btree_key_generator.cpp#L210) 


src/mongo/db/index/expression_keys_private.cpp
----
* 16754 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/expression_keys_private.cpp#L116) Can't parse geometry from element: 
* 16755 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/expression_keys_private.cpp#L122) Can't extract geo keys from object, malformed geometry?: 
* 16756 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/expression_keys_private.cpp#L125) Unable to generate keys for (likely malformed) geometry: 
* 16766 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/expression_keys_private.cpp#L320) Error: hashed indexes do not currently support array values
* 16767 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/expression_keys_private.cpp#L337) Only HashVersion 0 has been defined
* 16775 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/expression_keys_private.cpp#L352) latlng not an array
* 16776 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/expression_keys_private.cpp#L388) geo field is not a number
* 16804 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/expression_keys_private.cpp#L251) 


src/mongo/db/index/expression_params.h
----
* 16747 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/expression_params.h#L154) coarsestIndexedLevel must be >= 0
* 16748 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/expression_params.h#L155) finestIndexedLevel must be <= 30
* 16749 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/expression_params.h#L156) finestIndexedLevel must be >= coarsestIndexedLevel
* 16765 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/expression_params.h#L92) error: no hashed index field
* 16769 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/expression_params.h#L105) bucketSize cannot be zero
* 16770 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/expression_params.h#L113) can't have more than one geo field
* 16771 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/expression_params.h#L114) the geo field has to be first in index
* 16772 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/expression_params.h#L118) geoSearch can only have 1 non-geo field for now
* 16777 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/expression_params.h#L103) need bucketSize
* 16800 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/expression_params.h#L45) can't have 2 geo fields
* 16801 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/expression_params.h#L46) 2d has to be first in index
* 16802 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/expression_params.h#L57) no geo field specified
* 17395 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/expression_params.h#L159) 


src/mongo/db/index/hash_access_method.cpp
----
* 16763 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/hash_access_method.cpp#L42) Currently only single field hashed index supported.
* 16764 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/hash_access_method.cpp#L45) Currently hashed indexes cannot guarantee uniqueness. Use a regular index.


src/mongo/db/index/haystack_access_method.cpp
----
* 16773 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/haystack_access_method.cpp#L52) no geo field specified
* 16774 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/haystack_access_method.cpp#L53) no non-geo fields specified


src/mongo/db/index/s2_access_method.cpp
----
* 16750 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/s2_access_method.cpp#L71) Expect at least one geo field, spec=
* 16823 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/s2_access_method.cpp#L65) Cannot use 
* 17394 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/index/s2_access_method.cpp#L94) 


src/mongo/db/initialize_server_global_state.cpp
----
* 16447 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/initialize_server_global_state.cpp#L95) 
* 16448 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/initialize_server_global_state.cpp#L227) 


src/mongo/db/instance.cpp
----
* 10055 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L582) update object too large
* 10058 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L892) not master
* 10309 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1217) Unable to create/open lock file: " << name << ' ' << errnoWithDescription() << " Is a mongod instance already running?
* 10310 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1222) Unable to lock file: " + name + ". Is a mongod instance already running?
* 12596 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1287) old lock file
* 13004 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L533) sent negative cursors to kill: 
* 13073 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L735) shutting down
* 13342 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1305) Unable to truncate lock file
* 13597 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1297) can't start without --journal enabled when journal/ files are present
* 13618 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1322) can't start without --journal enabled when journal/ files are present
* 13625 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1301) Unable to truncate lock file
* 13627 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L1211) Unable to create/open lock file: " << name << ' ' << m << ". Is a mongod instance already running?
* 13658 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L532) bad kill cursors size: 
* 13659 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L531) sent 0 cursors to kill
* 16257 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L470) Invalid ns [" << ns << "]
* 16258 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/instance.cpp#L689) Invalid ns [" << ns << "]


src/mongo/db/introspect.cpp
----
* 16372 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/introspect.cpp#L162) 


src/mongo/db/jsobj.cpp
----
* 10060 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/jsobj.cpp#L616) woSortOrder needs a non-empty sortKey
* 10061 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/jsobj.cpp#L1292) type not supported for appendMinElementForType
* 10311 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/jsobj.cpp#L124) 
* 10312 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/jsobj.cpp#L321) 
* 12579 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/jsobj.cpp#L1010) unhandled cases in BSONObj okForStorage
* 14853 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/jsobj.cpp#L1345) type not supported for appendMaxElementForType


src/mongo/db/json.cpp
----
* 16619 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/json.cpp#L1214) 
* 17031 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/json.cpp#L1208) 


src/mongo/db/keypattern.cpp
----
* 16634 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/keypattern.cpp#L115) 
* 16649 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/keypattern.cpp#L110) 
* 17439 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/keypattern.cpp#L198) 


src/mongo/db/lasterror.cpp
----
* 13649 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/lasterror.cpp#L106) no operation yet


src/mongo/db/lockstat.cpp
----
* 16146 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/lockstat.cpp#L99) 
* 16339 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/lockstat.cpp#L110) 


src/mongo/db/lockstate.cpp
----
* 16115 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/lockstate.cpp#L224) 
* 16169 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/lockstate.cpp#L140) 
* 16170 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/lockstate.cpp#L259) 
* 16231 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/lockstate.cpp#L254) 


src/mongo/db/matcher/expression_leaf.cpp
----
* 16828 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher/expression_leaf.cpp#L161) 
* 17448 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher/expression_leaf.cpp#L139) 


src/mongo/db/matcher/expression_where.cpp
----
* 16812 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher/expression_where.cpp#L125) 
* 16813 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher/expression_where.cpp#L128) unknown error in invocation of $where function


src/mongo/db/matcher/matcher.cpp
----
* 16810 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/matcher/matcher.cpp#L49) 


src/mongo/db/namespace_string-inl.h
----
* 17235 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace_string-inl.h#L126) 
* 17246 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace_string-inl.h#L129) 
* 17295 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace_string-inl.h#L139) namespaces cannot have embedded null characters


src/mongo/db/namespace_string.h
----
* 10078 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace_string.h#L183) nsToDatabase: db too long
* 10088 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace_string.h#L186) nsToDatabase: db too long
* 16886 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/namespace_string.h#L204) nsToCollectionSubstring: no .


src/mongo/db/operation_context_impl.cpp
----
* 11601 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/operation_context_impl.cpp#L141) operation was interrupted


src/mongo/db/ops/delete_executor.cpp
----
* 10100 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/delete_executor.cpp#L99) cannot delete from collection with reserved $ in name
* 10101 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/delete_executor.cpp#L108) 
* 12050 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/delete_executor.cpp#L93) 
* 17417 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/delete_executor.cpp#L85) 


src/mongo/db/ops/update.cpp
----
* 10155 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L68) cannot update reserved $ collection
* 10156 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L72) 
* 16836 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L747) 
* 16837 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L586) 
* 16838 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L804) 
* 16839 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L810) 
* 17243 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L459) 
* 17278 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L666) 
* 17352 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L740) 
* 17354 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L737) 
* 17419 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L642) 
* 17420 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update.cpp#L778) 


src/mongo/db/ops/update_driver.cpp
----
* 16980 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/ops/update_driver.cpp#L433) 


src/mongo/db/pipeline/accumulator_sum.cpp
----
* 16000 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/accumulator_sum.cpp#L74) $sum resulted in a non-numeric type


src/mongo/db/pipeline/document.cpp
----
* 16486 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document.cpp#L98) 
* 16487 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document.cpp#L167) 
* 16488 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document.cpp#L315) 
* 16489 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document.cpp#L345) 
* 16490 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document.cpp#L142) Tried to make oversized document
* 16491 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document.cpp#L177) Tried to make oversized document
* 16601 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document.cpp#L299) 


src/mongo/db/pipeline/document_source_command_shards.cpp
----
* 16390 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_command_shards.cpp#L73) sharded pipeline failed on shard 
* 16391 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_command_shards.cpp#L80) no result array? shard:


src/mongo/db/pipeline/document_source_cursor.cpp
----
* 16028 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_cursor.cpp#L118) collection or index disappeared when cursor yielded
* 17285 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_cursor.cpp#L121) cursor encountered an error: 
* 17286 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_cursor.cpp#L124) Unexpected return from Runner::getNext: 
* 17392 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_cursor.cpp#L204) No _runner. Were we disposed before explained?


src/mongo/db/pipeline/document_source_geo_near.cpp
----
* 16602 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_geo_near.cpp#L60) $geoNear is only allowed as the first pipeline stage
* 16603 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_geo_near.cpp#L145) Already ran geoNearCommand
* 16604 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_geo_near.cpp#L151) geoNear command failed: 
* 16605 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_geo_near.cpp#L173) $geoNear requires a 'near' option as an Array
* 16606 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_geo_near.cpp#L178) $geoNear requires a 'distanceField' option as a String
* 16607 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_geo_near.cpp#L202) $geoNear requires that 'includeLocs' option is a String


src/mongo/db/pipeline/document_source_group.cpp
----
* 15947 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L244) a group's fields must be specified in an object
* 15948 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L259) a group's _id may only be specified once
* 15950 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L280) 
* 15951 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L285) 
* 15952 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L304) unknown group operator '" << key.name << "'
* 15953 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L315) 
* 15954 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L325) 
* 15955 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L332) a group specification must include an _id
* 16414 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L275) 
* 16945 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L361) Exceeded memory limit for $group, but didn't allow external sort.
* 17030 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L265) $doingMerge should be true if present
* 17390 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_group.cpp#L515) $group does not support inclusion-style expressions


src/mongo/db/pipeline/document_source_limit.cpp
----
* 15957 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_limit.cpp#L93) the limit must be specified as a number
* 15958 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_limit.cpp#L85) the limit must be positive


src/mongo/db/pipeline/document_source_match.cpp
----
* 15959 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_match.cpp#L332) the match filter must be an expression in an object
* 16395 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_match.cpp#L317) $where is not allowed inside of a $match aggregation expression
* 16424 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_match.cpp#L320) $near is not allowed inside of a $match aggregation expression
* 16426 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_match.cpp#L322) $nearSphere is not allowed inside of a $match aggregation expression
* 17309 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_match.cpp#L56) Should never call getNext on a $match stage with $text clause
* 17313 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_match.cpp#L296) $match with $text is only allowed as the first pipeline stage


src/mongo/db/pipeline/document_source_merge_cursors.cpp
----
* 17026 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_merge_cursors.cpp#L65) Expected an Array, but got a 
* 17027 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_merge_cursors.cpp#L71) Expected an Object, but got a 
* 17028 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_merge_cursors.cpp#L126) 


src/mongo/db/pipeline/document_source_out.cpp
----
* 16990 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_out.cpp#L169) $out only supports a string argument, not 
* 16994 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_out.cpp#L77) failed to create temporary $out collection '
* 16995 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_out.cpp#L92) copying index for $out failed.
* 16996 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_out.cpp#L102) insert for $out failed: 
* 16997 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_out.cpp#L147) renameCollection for $out failed: 
* 17000 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_out.cpp#L180) $out shouldn't have different db than input
* 17017 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_out.cpp#L58) namespace '
* 17018 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_out.cpp#L137) namespace '
* 17152 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_out.cpp#L63) namespace '
* 17385 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_out.cpp#L174) Can't $out to special collection: 


src/mongo/db/pipeline/document_source_project.cpp
----
* 15969 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_project.cpp#L90) 
* 16402 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_project.cpp#L104) parseObject() returned wrong type of Expression
* 16403 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_project.cpp#L105) $projection requires at least one output field


src/mongo/db/pipeline/document_source_redact.cpp
----
* 17053 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_redact.cpp#L130) $redact's expression should not return anything 


src/mongo/db/pipeline/document_source_skip.cpp
----
* 15956 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_skip.cpp#L102) 
* 15972 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_skip.cpp#L94) 


src/mongo/db/pipeline/document_source_sort.cpp
----
* 15973 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_sort.cpp#L144)  the 
* 15974 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_sort.cpp#L179) $sort key ordering must be specified using a number or {$meta: 'text'}
* 15975 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_sort.cpp#L184) $sort key ordering must be 1 (for ascending) or -1 (for descending)
* 15976 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_sort.cpp#L190)  must have at least one sort key
* 17196 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_sort.cpp#L228) can only mergePresorted from MergeCursors and CommandShards
* 17312 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_sort.cpp#L170) 


src/mongo/db/pipeline/document_source_unwind.cpp
----
* 15978 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_unwind.cpp#L87) Value at end of $unwind field path '
* 15979 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_unwind.cpp#L153) can't unwind more than one path
* 15981 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/document_source_unwind.cpp#L166) the 


src/mongo/db/pipeline/expression.cpp
----
* 15982 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L184) 
* 15983 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L215) 
* 15990 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L229) this object is already an operator expression, and can't be used as a document expression (at '
* 15992 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L289) 
* 15999 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L330) invalid operator '" << opName << "'
* 16034 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2188) 
* 16035 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2194) 
* 16400 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1105) 
* 16401 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1123) 
* 16404 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L220) $expressions are not allowed at the top-level of $project
* 16405 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L233) dotted field names are only allowed at the top level
* 16406 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L281) 
* 16407 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L929) inclusion not supported in objects nested in $expressions
* 16417 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L432) $add resulted in a non-numeric type
* 16418 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1665) $multiply resulted in a non-numeric type
* 16419 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L180) field path must not contain embedded null characters" << prefixedField.find("\0") << ",
* 16420 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L276) field inclusion is not allowed inside of $expressions
* 16554 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L412) $add only supports numeric or date types, not 
* 16555 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1653) $multiply only supports numeric types, not 
* 16556 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2257) cant $subtract a
* 16608 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L850) can't $divide by zero
* 16609 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L859) $divide only supports numeric types, not 
* 16610 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1573) can't $mod by 0
* 16611 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1601) $mod only supports numeric types, not 
* 16612 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L399) only one Date allowed in an $add expression
* 16613 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2251) cant $subtract a 
* 16702 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L698) $concat only supports strings, not 
* 16866 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L60) empty variable names are not allowed
* 16867 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L67) 
* 16868 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L79) '" << varName << "' contains an invalid character 
* 16869 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L86) empty variable names are not allowed
* 16870 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L94) 
* 16871 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L106) '" << varName << "' contains an invalid character 
* 16872 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1183) '$' by itself is not a valid FieldPath
* 16873 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1180) FieldPath '" << raw << "' doesn't start with $
* 16874 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1295) $let only supports an object as it's argument
* 16875 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1308) 
* 16876 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1313) Missing 'vars' parameter to $let
* 16877 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1315) Missing 'in' parameter to $let
* 16878 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1401) $map only supports an object as it's argument
* 16879 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1417) 
* 16880 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1422) Missing 'input' parameter to $map
* 16881 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1424) Missing 'as' parameter to $map
* 16882 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1426) Missing 'in' parameter to $map
* 16883 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1474) input to $map must be an Array not 
* 17040 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L445) 's argument must be an array, but is 
* 17041 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L541) 's argument must be an array, but is 
* 17042 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2054) both operands of $setIsSubset must be arrays. Second 
* 17043 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2125) All operands of $setUnion must be arrays. One argument
* 17044 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1958) All operands of $setEquals must be arrays. One 
* 17045 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1947) $setEquals needs at least two arguments had: 
* 17046 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2051) both operands of $setIsSubset must be arrays. First 
* 17047 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1991) All operands of $setIntersection must be arrays. One 
* 17048 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1918) both operands of $setDifference must be arrays. First 
* 17049 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1921) both operands of $setDifference must be arrays. Second 
* 17064 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L315) Duplicate expression (" << key << ") detected at 
* 17080 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L747) Missing 'if' parameter to $cond
* 17081 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L749) Missing 'then' parameter to $cond
* 17082 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L751) Missing 'else' parameter to $cond
* 17083 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L742) 
* 17124 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2145) The argument to $size must be an Array, but was of type: 
* 17199 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L113) can't use Variables::setValue to set ROOT
* 17275 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L142) Can't redefine ROOT
* 17276 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L155) Use of undefined variable: 
* 17307 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1510) $meta only supports String arguments
* 17308 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L1512) Unsupported argument to $meta: 
* 17310 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2079) both operands of $setIsSubset must be arrays. First 
* 17311 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.cpp#L2100) both operands of $setIsSubset must be arrays. Second 


src/mongo/db/pipeline/expression.h
----
* 16020 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/expression.h#L354) 


src/mongo/db/pipeline/field_path.cpp
----
* 15998 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/field_path.cpp#L102) FieldPath field names may not be empty strings.
* 16409 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/field_path.cpp#L42) FieldPath cannot be constructed from an empty vector.
* 16410 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/field_path.cpp#L103) FieldPath field names may not start with '$'.
* 16411 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/field_path.cpp#L104) FieldPath field names may not contain '\0'.
* 16412 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/field_path.cpp#L106) FieldPath field names may not contain '.'.


src/mongo/db/pipeline/pipeline.cpp
----
* 15942 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/pipeline.cpp#L178) pipeline element 
* 16389 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/pipeline.cpp#L507) 
* 16435 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/pipeline.cpp#L184) A pipeline stage specification object must contain exactly one field.
* 16436 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/pipeline.cpp#L196) 
* 16600 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/pipeline.cpp#L478) should not have an empty pipeline
* 16949 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/pipeline.cpp#L151) 
* 16991 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/pipeline.cpp#L206) $out can only be the final stage in the pipeline
* 17138 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/pipeline.cpp#L327) 
* 17139 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/pipeline.cpp#L338) 


src/mongo/db/pipeline/value.cpp
----
* 16003 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L368) 
* 16004 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L387) 
* 16005 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L406) 
* 16006 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L422) 
* 16007 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L507) 
* 16378 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L520) 
* 16421 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L440) Can't handle date values outside of time_t range
* 16422 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L461) gmtime failed - your system doesn't support dates before 1970
* 16423 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L464) gmtime failed to convert time_t of 
* 16485 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L106) 
* 16557 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/value.cpp#L663) can't compare CodeWScope values containing a NUL byte in the code.


src/mongo/db/prefetch.cpp
----
* 16427 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/prefetch.cpp#L174) 


src/mongo/db/projection.cpp
----
* 10053 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L115) You cannot currently mix including and excluding fields. 
* 10371 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L41) can only add to Projection once
* 13097 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L98) Unsupported projection option: 
* 13098 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L76) $slice only supports numbers and [skip, limit] arrays
* 13099 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L66) $slice array wrong size
* 13100 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L71) $slice limit must be positive
* 16342 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L81) elemMatch: invalid argument.  object required.
* 16343 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L83) Cannot specify positional operator and $elemMatch
* 16344 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L86) Cannot use $elemMatch projection on a nested field
* 16345 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L122) Cannot exclude array elements with the positional operator
* 16346 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L124) Cannot specify more than one positional array element per query
* 16347 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L126) Cannot specify positional operator and $elemMatch
* 16348 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L189) matchers are only supported for $elemMatch
* 16349 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L199) $elemMatch specified, but projection field not found.
* 16350 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L203) $elemMatch called on document element with eoo
* 16351 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L205) $elemMatch called on array element with eoo
* 16352 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L302) positional operator (
* 16353 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L307) positional operator element mismatch
* 16354 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/projection.cpp#L358) Positional operator does not match the query specifier.


src/mongo/db/query/new_find.cpp
----
* 13530 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/query/new_find.cpp#L439) bad or malformed command request?
* 16256 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/query/new_find.cpp#L417) Invalid ns [" << ns << "]
* 16332 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/query/new_find.cpp#L414) can't have an empty ns
* 16979 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/query/new_find.cpp#L428) bad numberToReturn (
* 17007 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/query/new_find.cpp#L560) Unable to execute query: 
* 17011 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/query/new_find.cpp#L190) auth error
* 17144 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/query/new_find.cpp#L709) Runner error: 
* 17287 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/query/new_find.cpp#L470) Can't canonicalize query: 
* 17356 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/query/new_find.cpp#L157) collection dropped between getMore calls
* 17406 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/query/new_find.cpp#L272) getMore runner error: 
* 17510 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/query/new_find.cpp#L504) Explain error: 


src/mongo/db/repl/bgsync.cpp
----
* 1000 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/bgsync.cpp#L209) replSet source for syncing doesn't seem to be await capable -- is it an older version of mongodb?
* 16235 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/bgsync.cpp#L484) going to start syncing, but buffer is not empty


src/mongo/db/repl/master_slave.cpp
----
* 10002 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/master_slave.cpp#L272) local.sources collection corrupt?
* 10118 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/master_slave.cpp#L99) 'host' field not set in sources collection object
* 10119 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/master_slave.cpp#L100) only source='main' allowed for now with replication", sourceName() == "main
* 10120 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/master_slave.cpp#L103) bad sources 'syncedTo' field value
* 10123 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/master_slave.cpp#L949) replication error last applied optime at slave >= nextOpTime from master
* 10384 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/master_slave.cpp#L283) --only requires use of --source
* 10385 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/master_slave.cpp#L344) Unable to get database list
* 10386 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/master_slave.cpp#L687) non Date ts found: 
* 10389 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/master_slave.cpp#L758) Unable to get database list
* 10390 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/master_slave.cpp#L848) got $err reading remote oplog
* 10391 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/master_slave.cpp#L853) repl: bad object read from remote oplog
* 13344 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/master_slave.cpp#L844) trying to slave off of a non-master
* 14032 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/master_slave.cpp#L472) Invalid 'ts' in remote log
* 14033 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/master_slave.cpp#L478) Unable to get database list
* 14034 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/master_slave.cpp#L520) Duplicate database names present after attempting to delete duplicates
* 15914 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/master_slave.cpp#L531) 
* 17065 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/master_slave.cpp#L271) Internal error reading from local.sources
* 17066 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/master_slave.cpp#L308) Internal error reading from local.sources


src/mongo/db/repl/oplog.cpp
----
* 13257 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/oplog.cpp#L468) 
* 13288 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/oplog.cpp#L111) replSet error write op to db before replSet initialized", str::startsWith(ns, "local.
* 13347 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/oplog.cpp#L289) local.oplog.rs missing. did you drop it? if so restart server
* 13389 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/oplog.cpp#L130) 
* 14825 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/oplog.cpp#L758) error in applyOperation : unknown opType 
* 17322 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/oplog.cpp#L98) 
* 17405 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/oplog.cpp#L253) 


src/mongo/db/repl/oplogreader.h
----
* 15910 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/oplogreader.h#L131) Doesn't have cursor for reading oplog
* 15911 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/oplogreader.h#L136) Doesn't have cursor for reading oplog


src/mongo/db/repl/repl_coordinator_impl.cpp
----
* 18508 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/repl_coordinator_impl.cpp#L188) 


src/mongo/db/repl/repl_set_impl.cpp
----
* 13290 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/repl_set_impl.cpp#L450) bad replSet oplog entry?
* 13302 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/repl_set_impl.cpp#L583) replSet error self appears twice in the repl set configuration
* 16844 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/repl_set_impl.cpp#L170) 
* 17347 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/repl_set_impl.cpp#L458) Problem reading earliest entry from oplog


src/mongo/db/repl/repl_set_seed_list.cpp
----
* 13093 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/repl_set_seed_list.cpp#L56) 
* 13096 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/repl_set_seed_list.cpp#L77) bad --replSet command line config string - dups?
* 13101 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/repl_set_seed_list.cpp#L80) can't use localhost in replset host list
* 13114 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/repl_set_seed_list.cpp#L75) bad --replSet seed hostname


src/mongo/db/repl/replset_commands.cpp
----
* 16888 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/replset_commands.cpp#L422) optimes field should be an array with an object for each secondary


src/mongo/db/repl/rs_config.cpp
----
* 13107 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L554) 
* 13108 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L564) bad replset config -- duplicate hosts in the config object?
* 13109 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L711) multiple rows in " << rsConfigNs << " not supported host: 
* 13115 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L475) bad " + rsConfigNs + " config: version
* 13117 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L571) bad " + rsConfigNs + " config
* 13122 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L627) bad repl set config?
* 13126 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L178) bad Member config
* 13131 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L485) replSet error parsing (or missing) 'members' field in config object
* 13132 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L328) 
* 13133 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L333) replSet bad config no members
* 13135 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L560) 
* 13260 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L686) 
* 13308 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L332) replSet bad config version #
* 13309 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L334) replSet bad config maximum number of members is 12
* 13393 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L570) can't use localhost in repl set member names except when using it for all members
* 13419 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L185) priorities must be between 0.0 and 1000
* 13432 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L280) _id may not change for members
* 13433 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L297) can't find self in new replset config
* 13434 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L60) unexpected field '" << e.fieldName() << "' in object
* 13437 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L186) slaveDelay requires priority be zero
* 13438 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L187) bad slaveDelay value
* 13439 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L188) priority must be 0 when hidden=true
* 13476 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L284) buildIndexes may not change for members
* 13477 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L189) priority must be 0 when buildIndexes=false
* 13510 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L290) arbiterOnly may not change for members
* 13612 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L346) replSet bad config maximum number of voting members is 7
* 13613 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L347) replSet bad config no voting members
* 13645 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L274) hosts cannot switch between localhost and hostname
* 14046 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L396) getLastErrorMode rules must be objects
* 14827 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L546) arbiters cannot have tags
* 14828 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L408) getLastErrorMode criteria must be greater than 0: 
* 14829 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L403) getLastErrorMode criteria must be numeric
* 14831 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L413) mode " << clauseObj << " requires 
* 16438 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L584) Heartbeat timeout must be non-negative
* 17492 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L190) arbiter must vote (cannot have 0 votes)
* 17505 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.cpp#L337) replSet illegal config: getLastErrorDefaults w:0


src/mongo/db/repl/rs_config.h
----
* 13112 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.h#L148) bad replset heartbeat option
* 13113 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_config.h#L149) bad replset heartbeat option


src/mongo/db/repl/rs_initialsync.cpp
----
* 13404 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_initialsync.cpp#L63) 
* 16233 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_initialsync.cpp#L85) 


src/mongo/db/repl/rs_initiate.cpp
----
* 13144 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_initiate.cpp#L152) 
* 13145 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_initiate.cpp#L115) set name does not match the set name host " + i->h.toString() + " expects
* 13256 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_initiate.cpp#L119) member " + i->h.toString() + " is already initiated
* 13259 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_initiate.cpp#L105) 
* 13278 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_initiate.cpp#L80) bad config: isSelf is true for multiple hosts: 
* 13279 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_initiate.cpp#L86) 
* 13311 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_initiate.cpp#L158) member " + i->h.toString() + " has data already, cannot initiate set.  All members except initiator must be empty.
* 13341 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_initiate.cpp#L124) member " + i->h.toString() + " has a config version >= to the new cfg version; cannot change config
* 13420 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_initiate.cpp#L73) initiation and reconfiguration of a replica set must be sent to a node that can become primary


src/mongo/db/repl/rs_rollback.cpp
----
* 13410 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_rollback.cpp#L390) replSet too much data to roll back
* 13423 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_rollback.cpp#L511) 
* 15908 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_rollback.cpp#L355) 
* 15909 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/rs_rollback.cpp#L456) replSet rollback error resyncing collection 


src/mongo/db/repl/sync.cpp
----
* 15916 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/sync.cpp#L104) 
* 15917 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/sync.cpp#L132) 


src/mongo/db/repl/sync_source_feedback.cpp
----
* 17447 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/sync_source_feedback.cpp#L130) upstream updater is not supported by the member from which we


src/mongo/db/repl/sync_tail.cpp
----
* 12000 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/sync_tail.cpp#L510) rs slaveDelay differential too big check clocks and systems
* 16397 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/sync_tail.cpp#L138) 
* 16620 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/sync_tail.cpp#L296) there are ops to sync, but I'm primary


src/mongo/db/repl/topology_coordinator_impl.cpp
----
* 18505 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/topology_coordinator_impl.cpp#L566) 


src/mongo/db/repl/topology_coordinator_impl.h
----
* 17490 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/topology_coordinator_impl.h#L199) bad replset heartbeat option
* 17491 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/topology_coordinator_impl.h#L200) bad replset heartbeat option


src/mongo/db/repl/write_concern.cpp
----
* 16250 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/write_concern.cpp#L172) w has to be a string or a number


src/mongo/db/restapi.cpp
----
* 13085 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/restapi.cpp#L171) query failed for dbwebserver


src/mongo/db/sorter/sorter.cpp
----
* 16814 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/sorter/sorter.cpp#L168) error opening file \"" << _fileName << "\": 
* 16815 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/sorter/sorter.cpp#L172) unexpected empty file: 
* 16816 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/sorter/sorter.cpp#L212) file too short?
* 16817 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/sorter/sorter.cpp#L245) error reading file \"
* 16818 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/sorter/sorter.cpp#L801) error opening file \"" << _fileName << "\": 
* 16819 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/sorter/sorter.cpp#L456) 
* 16820 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/sorter/sorter.cpp#L727) 
* 16821 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/sorter/sorter.cpp#L842) error writing to file \"" << _fileName << "\": 
* 16946 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/sorter/sorter.cpp#L786) Attempting to use external sort from mongos. This is not allowed.
* 16947 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/sorter/sorter.cpp#L876) Attempting to use external sort from mongos. This is not allowed.
* 17061 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/sorter/sorter.cpp#L222) couldn't get uncompressed length
* 17062 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/sorter/sorter.cpp#L226) decompression failed
* 17148 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/sorter/sorter.cpp#L789) Attempting to use external sort without setting SortOptions::tempDir
* 17149 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/sorter/sorter.cpp#L879) Attempting to use external sort without setting SortOptions::tempDir


src/mongo/db/storage/mmap_v1/data_file.cpp
----
* 10084 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/data_file.cpp#L49) can't map file memory - mongo requires 64 bit build for larger datasets
* 10085 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/data_file.cpp#L51) can't map file memory
* 10357 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/data_file.cpp#L164) shutdown in progress
* 10359 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/data_file.cpp#L165) header==0 on new extent: 32 bit mmap space exceeded?
* 13440 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/data_file.cpp#L73) bad offset:
* 13441 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/data_file.cpp#L67) bad offset:
* 13640 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/data_file.cpp#L184) DataFileHeader looks corrupt at file open filelength:" << filelength << " fileno:


src/mongo/db/storage/mmap_v1/dur.cpp
----
* 13599 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/dur.cpp#L446) Written data does not match in-memory view. Missing WriteIntent?
* 13616 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/dur.cpp#L217) can't disable durability with pending writes
* 16434 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/dur.cpp#L319) 
* 18507 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/dur.cpp#L790) File is closing while there are unwritten changes.


src/mongo/db/storage/mmap_v1/dur_journal.cpp
----
* 13611 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/dur_journal.cpp#L601) can't read lsn file in journal directory : 
* 13614 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/dur_journal.cpp#L568) unexpected version number of lsn file in journal/ directory got: 
* 15926 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/dur_journal.cpp#L413) Insufficient free space for journals


src/mongo/db/storage/mmap_v1/dur_preplogbuffer.cpp
----
* 17388 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/dur_preplogbuffer.cpp#L146) 


src/mongo/db/storage/mmap_v1/dur_recover.cpp
----
* 13531 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/dur_recover.cpp#L93) unexpected files in journal directory " << dir.string() << " : 
* 13532 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/dur_recover.cpp#L100) 
* 13533 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/dur_recover.cpp#L177) problem processing journal file during recovery
* 13535 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/dur_recover.cpp#L536) recover abrupt journal file end
* 13536 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/dur_recover.cpp#L460) journal version number mismatch 
* 13537 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/dur_recover.cpp#L451) 
* 13544 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/dur_recover.cpp#L517) recover error couldn't open 
* 13545 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/dur_recover.cpp#L543) --durOptions 
* 13594 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/dur_recover.cpp#L424) journal checksum doesn't match
* 13622 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/dur_recover.cpp#L287) Trying to write past end of file in WRITETODATAFILES
* 15874 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/dur_recover.cpp#L128) couldn't uncompress journal section


src/mongo/db/storage/mmap_v1/durable_mapped_file.cpp
----
* 13520 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/durable_mapped_file.cpp#L133) DurableMappedFile only supports filenames in a certain format 
* 13636 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/durable_mapped_file.cpp#L162) file " << filename() << " open/create failed in createPrivateMap (look in log for more information)
* 16112 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/durable_mapped_file.cpp#L60) 


src/mongo/db/storage/mmap_v1/durop.cpp
----
* 13546 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/durop.cpp#L64) journal recover: unrecognized opcode in journal 
* 13547 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/durop.cpp#L155) recover couldn't create file 
* 13628 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/durop.cpp#L169) recover failure writing file 


src/mongo/db/storage/mmap_v1/mmap_v1_database_catalog_entry.cpp
----
* 16966 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/mmap_v1_database_catalog_entry.cpp#L71) _extentManager.init failed: 
* 17489 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/mmap_v1_database_catalog_entry.cpp#L551) 


src/mongo/db/storage/mmap_v1/mmap_v1_extent_manager.cpp
----
* 10295 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/mmap_v1_extent_manager.cpp#L127) getFile(): bad file number value (corrupt db?).
* 10358 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/mmap_v1_extent_manager.cpp#L256) bad new extent size
* 12501 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/mmap_v1_extent_manager.cpp#L232) quota exceeded
* 14026 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/mmap_v1_extent_manager.cpp#L558) 
* 14810 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/mmap_v1_extent_manager.cpp#L308) couldn't allocate space for a new extent


src/mongo/db/storage/mmap_v1/repair_database.cpp
----
* 17402 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/storage/mmap_v1/repair_database.cpp#L256) 


src/mongo/db/structure/btree/btree_logic.cpp
----
* 17433 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/btree/btree_logic.cpp#L2225) _insert: reuse key but lchild is not null
* 17434 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/btree/btree_logic.cpp#L2226) _insert: reuse key but rchild is not null
* 17435 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/btree/btree_logic.cpp#L340) n==0 in btree popBack()
* 17436 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/btree/btree_logic.cpp#L349) rchild not null in btree popBack()


src/mongo/db/structure/catalog/namespace-inl.h
----
* 10080 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace-inl.h#L45) ns name too long, max size is 127 bytes
* 10348 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace-inl.h#L55) $extra: ns name too long
* 17380 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace-inl.h#L46) ns name can't contain embedded '\0' byte


src/mongo/db/structure/catalog/namespace_details.cpp
----
* 10350 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace_details.cpp#L100) allocExtra: base ns missing?
* 10351 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace_details.cpp#L101) allocExtra: extra already exists
* 13283 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace_details.cpp#L131) Missing Extra
* 14045 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace_details.cpp#L132) missing Extra
* 14823 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace_details.cpp#L139) missing extra
* 14824 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace_details.cpp#L140) missing Extra
* 16499 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace_details.cpp#L218) 
* 17421 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace_details.cpp#L156) Missing Extra
* 17422 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace_details.cpp#L157) missing Extra
* 17423 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace_details.cpp#L164) missing extra
* 17424 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace_details.cpp#L165) missing Extra


src/mongo/db/structure/catalog/namespace_details_collection_entry.cpp
----
* 16631 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace_details_collection_entry.cpp#L315) index does not have an 'expireAfterSeconds' field
* 16632 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace_details_collection_entry.cpp#L327) current 'expireAfterSeconds' is not a number
* 17202 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace_details_collection_entry.cpp#L269) 


src/mongo/db/structure/catalog/namespace_details_rsv1_metadata.cpp
----
* 17486 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace_details_rsv1_metadata.cpp#L218) 
* 17488 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace_details_rsv1_metadata.cpp#L222) 


src/mongo/db/structure/catalog/namespace_index.cpp
----
* 10079 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace_index.cpp#L157) bad .ns file length, cannot open database
* 10081 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace_index.cpp#L71) too many namespaces/collections
* 10343 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace_index.cpp#L164) bad storageGlobalParams.lenForNewNsFiles
* 17315 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/catalog/namespace_index.cpp#L69) no . in ns


src/mongo/db/structure/record_store_v1_base.cpp
----
* 16484 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/record_store_v1_base.cpp#L878) 
* 17208 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/record_store_v1_base.cpp#L263) 
* 17210 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/record_store_v1_base.cpp#L270) 
* 17319 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/record_store_v1_base.cpp#L226) 
* 17441 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/record_store_v1_base.cpp#L193) 
* 17442 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/record_store_v1_base.cpp#L203) 


src/mongo/db/structure/record_store_v1_capped.cpp
----
* 13415 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/record_store_v1_capped.cpp#L475) emptying the collection is not allowed
* 17438 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/record_store_v1_capped.cpp#L679) 


src/mongo/db/structure/record_store_v1_simple.cpp
----
* 16469 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/record_store_v1_simple.cpp#L108) 
* 17437 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/db/structure/record_store_v1_simple.cpp#L343) 


src/mongo/dbtests/jsobjtests.cpp
----
* 12528 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/dbtests/jsobjtests.cpp#L1835) should be ok for storage:
* 12529 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/dbtests/jsobjtests.cpp#L1845) should NOT be ok for storage:


src/mongo/dbtests/mock/mock_conn_registry.cpp
----
* 16533 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/dbtests/mock/mock_conn_registry.cpp#L63) 
* 16534 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/dbtests/mock/mock_conn_registry.cpp#L80) 


src/mongo/dbtests/mock/mock_remote_db_server.cpp
----
* 16430 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/dbtests/mock/mock_remote_db_server.cpp#L153) no reply for cmd: 


src/mongo/dbtests/mock/mock_replica_set.cpp
----
* 16578 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/dbtests/mock/mock_replica_set.cpp#L113) 
* 16579 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/dbtests/mock/mock_replica_set.cpp#L116) 


src/mongo/s/balance.cpp
----
* 13258 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/balance.cpp#L470) oids broken after resetting!
* 16356 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/balance.cpp#L328) 


src/mongo/s/chunk.cpp
----
* 10163 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L147) can only handle numbers here - which i think is correct
* 10165 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L314) can't split as shard doesn't have a manager
* 10167 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L360) can't move shard to its current location!
* 10169 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L521) datasize failed!" , conn->runCommand( "admin
* 10170 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L92) Chunk needs a ns
* 10171 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L95) Chunk needs a server
* 10172 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L97) Chunk needs a min
* 10173 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L98) Chunk needs a max
* 10174 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L1376) config servers not all up
* 10412 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L489) 
* 13003 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L317) can't split a chunk with only one distinct value
* 13141 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L1139) Chunk map pointed to incorrect chunk
* 13282 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L759) Couldn't load a valid config for " + _ns + " after 3 attempts. Please try again.
* 13327 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L93) Chunk ns must match server ns
* 13331 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L1374) collection's metadata is undergoing changes. Please try again.
* 13332 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L315) need a split key to split chunk
* 13333 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L316) can't split a chunk in that many parts
* 13345 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L207) 
* 13449 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L1081) collection " << _ns << " already sharded with 
* 13501 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L1179) use geoNear command rather than $near query
* 13503 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L178) 
* 13507 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L1220) no chunks found between bounds " << min << " and 
* 14022 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L1371) Error locking distributed lock for chunk drop.
* 15903 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L1109) 
* 16068 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L1208) no chunk ranges available
* 16338 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L1410) 
* 17001 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L1424) could not drop chunks for 
* 8070 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L1143) 
* 8071 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk.cpp#L1444) cleaning up after drop failed: 


src/mongo/s/client_info.cpp
----
* 13134 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/client_info.cpp#L90) 
* 16472 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/client_info.cpp#L107) A ClientInfo already exists for this thread
* 16483 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/client_info.cpp#L121) 


src/mongo/s/cluster_client_internal.cpp
----
* 16624 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/cluster_client_internal.cpp#L406) operation failed
* 16625 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/cluster_client_internal.cpp#L414) cursor not found, transport error


src/mongo/s/commands_public.cpp
----
* 10420 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L1324) how could chunk manager be null!
* 12594 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L994) how could chunk manager be null!
* 13002 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L1136) shard internal error chunk manager should never be null
* 13091 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L1395) how could chunk manager be null!
* 13137 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L778) Source and destination collections must be on same shard
* 13138 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L772) You can't rename a sharded collection
* 13139 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L773) You can't rename to a sharded collection
* 13140 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L771) Don't recognize source or target DB
* 13343 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L1139) query for sharded findAndModify must have shardkey
* 13398 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L800) cant copy to sharded DB
* 13399 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L808) need a fromdb argument
* 13400 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L811) don't know where source DB is
* 13401 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L812) cant copy from sharded DB
* 13402 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L797) need a todb argument
* 13405 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L1194) min value " << min << " does not have shard key
* 13406 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L1196) max value " << max << " does not have shard key
* 13407 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L1186) how could chunk manager be null!
* 13408 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L1192) keyPattern must equal shard key
* 13500 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L1510) how could chunk manager be null!
* 15920 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L1749) Cannot output to a non-sharded collection, a sharded collection exists
* 16246 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L1462) Shard " + conf->getName() + " is too old to support GridFS sharded by {files_id:1, n:1}
* 16618 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L2529) 
* 17014 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L2486) aggregate command didn't return results on host: 
* 17015 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L2205) getDBConfig shouldn't return NULL
* 17016 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L2477) should only be running an aggregate command here
* 17020 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L2313) All shards must support cursors to get a cursor back from aggregation
* 17021 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L2316) All shards must support cursors to support new features in aggregation
* 17023 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L2375) 
* 17024 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L2379) 
* 17025 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L2383) 
* 17403 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L2417) Shard 
* 17404 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/commands_public.cpp#L2421) Shard 


src/mongo/s/config.cpp
----
* 10178 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L174) no primary!
* 10181 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L365) not sharded:
* 10184 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L741) _dropShardedCollections too many collections - bailing
* 10187 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L785) need configdbs
* 10189 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L1016) should only have 1 thing in config.version
* 13396 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L590) DBConfig save failed: 
* 13473 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L135) failed to save collection (
* 13509 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L509) can't migrate from 1.5.x release to the current one; need to upgrade to 1.6.x first
* 13648 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L194) can't shard collection because not all config servers are up
* 14822 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L449) state changed in the middle: 
* 15883 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L481) not sharded after chunk manager reset : 
* 15885 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L396) not sharded after reloading from chunks : 
* 8042 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L193) db doesn't have sharding enabled
* 8043 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.cpp#L202) collection already sharded


src/mongo/s/config.h
----
* 10190 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.h#L221) ConfigServer not setup
* 8041 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config.h#L164) no primary shard configured for db: 


src/mongo/s/config_upgrade.cpp
----
* 16621 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config_upgrade.cpp#L159) 
* 16622 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config_upgrade.cpp#L164) 
* 16623 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config_upgrade.cpp#L167) 
* 16729 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/config_upgrade.cpp#L318) 


src/mongo/s/cursors.cpp
----
* 10191 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/cursors.cpp#L117) cursor already done
* 13286 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/cursors.cpp#L322) sent 0 cursors to kill
* 13287 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/cursors.cpp#L323) too many cursors to kill


src/mongo/s/d_logic.cpp
----
* 10422 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_logic.cpp#L120) write with bad shard config and no server id!
* 16437 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_logic.cpp#L126) data size of operation is too large to queue for writeback
* 9517 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_logic.cpp#L116) cannot queue a writeback operation to the writeback queue


src/mongo/s/d_migrate.cpp
----
* 16976 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_migrate.cpp#L1808) 
* 16977 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_migrate.cpp#L2060) 


src/mongo/s/d_split.cpp
----
* 13593 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_split.cpp#L816) 


src/mongo/s/d_state.cpp
----
* 13298 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_state.cpp#L147) 
* 16855 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_state.cpp#L217) 
* 16857 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_state.cpp#L348) 
* 17004 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_state.cpp#L372) 
* 18509 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/d_state.cpp#L88) 


src/mongo/s/dbclient_multi_command.cpp
----
* 17255 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/dbclient_multi_command.cpp#L144) error receiving write command response, 


src/mongo/s/distlock.cpp
----
* 14023 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/distlock.cpp#L758) remote time in cluster " << _conn.toString() << " is now skewed, cannot force lock.
* 16060 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/distlock.cpp#L164) 


src/mongo/s/distlock_test.cpp
----
* 13678 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/distlock_test.cpp#L423) Could not communicate with server " << server.toString() << " in cluster " << cluster.toString() << " to change skew by 


src/mongo/s/grid.cpp
----
* 10185 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/grid.cpp#L158) can't find a shard to put new db on
* 10186 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/grid.cpp#L178) removeDB expects db name
* 15918 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/grid.cpp#L65) 


src/mongo/s/request.cpp
----
* 13644 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/request.cpp#L81) can't use 'local' database through mongos" , ! str::startsWith( getns() , "local.
* 16978 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/request.cpp#L116) bad numberToReturn (


src/mongo/s/s_only.cpp
----
* 16462 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/s_only.cpp#L53) 
* 16478 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/s_only.cpp#L79) Client being used for incoming connection thread in mongos


src/mongo/s/server.cpp
----
* 10197 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/server.cpp#L202) createDirectClient not implemented for sharding yet


src/mongo/s/shard.cpp
----
* 13128 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard.cpp#L189) can't find shard for: 
* 13129 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard.cpp#L171) can't find shard for: 
* 13136 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard.cpp#L402) 
* 13632 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard.cpp#L84) couldn't get updated shard list from config server
* 15847 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard.cpp#L462) can't authenticate to server 
* 15907 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard.cpp#L475) 
* 17363 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shard.cpp#L487) 


src/mongo/s/shardkey.h
----
* 13334 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/shardkey.h#L159) Shard Key must be less than 512 bytes


src/mongo/s/strategy.cpp
----
* 10200 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy.cpp#L102) mongos: error calling db
* 10204 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy.cpp#L482) dbgrid: getmore: error calling db
* 17012 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy.cpp#L459) duplicate sharded and unsharded cursor id 
* 17233 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy.cpp#L146) 
* 8010 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/strategy.cpp#L140) something is wrong, shouldn't see a command here


src/mongo/s/version_manager.cpp
----
* 10428 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/version_manager.cpp#L286) need_authoritative set but in authoritative mode already
* 10429 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/version_manager.cpp#L323) 
* 15904 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/version_manager.cpp#L90) cannot set version on invalid connection 
* 15905 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/version_manager.cpp#L95) cannot set version or shard on pair connection 
* 15906 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/version_manager.cpp#L98) cannot set version or shard on sync connection 
* 16334 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/version_manager.cpp#L101) cannot set version or shard on custom connection 


src/mongo/s/writeback_listener.cpp
----
* 13641 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/s/writeback_listener.cpp#L82) can't parse host [" << conn.getServerAddress() << "]


src/mongo/scripting/bench.cpp
----
* 15931 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/bench.cpp#L361) Authenticating to connection for _benchThread failed: 
* 15932 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/bench.cpp#L718) Authenticating to connection for benchThread failed: 
* 16147 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/bench.cpp#L237) Already finished.
* 16152 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/bench.cpp#L247) Cannot wait for state 
* 16157 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/bench.cpp#L214) 
* 16158 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/bench.cpp#L216) 
* 16164 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/bench.cpp#L182) loopCommands config not supported", args["loopCommands
* 16704 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/bench.cpp#L757) 
* 16705 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/bench.cpp#L788) 


src/mongo/scripting/engine.cpp
----
* 10206 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine.cpp#L100) can't append type from: 
* 10207 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine.cpp#L106) compile failed
* 10208 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine.cpp#L186) need to have locallyConnected already
* 10209 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine.cpp#L205) name has to be a string: 
* 10210 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine.cpp#L206) value has to be set
* 10430 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine.cpp#L179) invalid object id: not hex
* 10448 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine.cpp#L177) invalid object id: length
* 16669 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine.cpp#L197) unable to get db client cursor from query


src/mongo/scripting/engine.h
----
* 13474 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine.h#L229) no _getCurrentOpIdCallback
* 9004 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine.h#L112) invoke failed: 
* 9005 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine.h#L119) invoke failed: 


src/mongo/scripting/engine_v8.cpp
----
* 10230 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L695) can't handle external yet
* 10231 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L740) not an object
* 10232 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L983) not a function
* 12509 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L703) unable to get type of field 
* 12510 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L1194) externalSetup already called, can't call localConnect
* 12511 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L1198) 
* 12512 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L1228) localConnect already called, can't call externalSetup
* 13475 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L1089) 
* 16496 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L1367) V8: NULL Object template instantiated. 
* 16661 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L1490) can't handle type: 
* 16662 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L1661) unable to convert JavaScript property to mongo element 
* 16711 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L1033) 
* 16712 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L1049) 
* 16716 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L1617) cannot convert native function to BSON
* 16721 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L1108) 
* 16862 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L1012) Too many arguments. Max is 24
* 16863 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L1433) Error converting 
* 16864 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L1554) ObjectID.str must be exactly 24 chars long
* 16865 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L1556) ObjectID.str must only have hex characters [0-1a-fA-F]
* 16985 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L1606) 
* 17260 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L1699) Converting from JavaScript to BSON failed: 
* 17279 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L1600) 
* 17355 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.cpp#L1457) Error converting 


src/mongo/scripting/engine_v8.h
----
* 16722 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.h#L605) 
* 17184 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/engine_v8.h#L476) 


src/mongo/scripting/utils.cpp
----
* 10261 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/utils.cpp#L37) 
* 16259 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/utils.cpp#L56) 


src/mongo/scripting/v8_db.cpp
----
* 16467 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/v8_db.cpp#L70) 
* 16468 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/v8_db.cpp#L117) 
* 16660 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/v8_db.cpp#L652) arrayAccess is not a function
* 16666 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/v8_db.cpp#L128) string argument too long
* 16667 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/v8_db.cpp#L189) Unable to get db client connection
* 16858 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/v8_db.cpp#L79) Too many arguments. Max is 24
* 16859 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/v8_db.cpp#L136) Mongo function is only usable as a constructor
* 16860 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/v8_db.cpp#L168) Mongo function is only usable as a constructor
* 16861 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/v8_db.cpp#L630) getCollection returned something other than a collection


src/mongo/scripting/v8_utils.cpp
----
* 16696 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/v8_utils.cpp#L57) error converting js type to Utf8Value


src/mongo/scripting/v8_utils.h
----
* 16664 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/v8_utils.h#L43) 
* 16686 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/scripting/v8_utils.h#L88) error converting js type to Utf8Value


src/mongo/shell/shell_options.cpp
----
* 17396 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_options.cpp#L251) 


src/mongo/shell/shell_utils.cpp
----
* 10258 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L100) processinfo not supported
* 12513 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L233) connect failed", scope.exec( _dbConnect , "(connect)
* 12514 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L236) login failed", scope.exec( _dbAuth , "(auth)
* 12518 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L117) srand requires a single numeric argument
* 12519 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L128) rand accepts no arguments
* 12597 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L75) need to specify 1 argument
* 13006 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L139) isWindows accepts no arguments
* 16453 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L195) interpreterVersion accepts no arguments
* 16822 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L148) getBuildInfo accepts no arguments
* 17134 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L174) replMonitorStats requires a single string argument (the ReplSet name)
* 17428 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils.cpp#L155) keyTooLarge takes exactly 2 arguments


src/mongo/shell/shell_utils_extended.cpp
----
* 10257 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils_extended.cpp#L58) need to specify 1 argument to listFiles
* 12581 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils_extended.cpp#L67) 
* 13301 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils_extended.cpp#L163) cat() : file to big to load as a variable
* 13411 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils_extended.cpp#L236) getHostName accepts no arguments
* 13619 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils_extended.cpp#L221) fuzzFile takes 2 arguments
* 13620 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils_extended.cpp#L224) couldn't open file to fuzz
* 16830 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils_extended.cpp#L115) 
* 16831 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils_extended.cpp#L118) 
* 16832 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils_extended.cpp#L132) 
* 16833 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils_extended.cpp#L191) 
* 16834 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils_extended.cpp#L194) 


src/mongo/shell/shell_utils_launcher.cpp
----
* 14042 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils_launcher.cpp#L420) 
* 15852 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils_launcher.cpp#L801) stopMongoByPid needs a number
* 15853 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils_launcher.cpp#L792) stopMongo needs a number
* 16701 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/shell/shell_utils_launcher.cpp#L262) 


src/mongo/tools/dump.cpp
----
* 10262 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/dump.cpp#L126) couldn't open file
* 14035 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/dump.cpp#L79) couldn't write to file
* 15933 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/dump.cpp#L167) Couldn't open file: 
* 17368 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/dump.cpp#L184) 
* 17369 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/dump.cpp#L433) 


src/mongo/tools/import.cpp
----
* 10263 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/import.cpp#L130) unknown error reading file
* 13289 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/import.cpp#L140) Invalid UTF8 character detected
* 13293 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/import.cpp#L179) Invalid JSON passed to mongoimport: 
* 13504 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/import.cpp#L212) BSON representation of supplied JSON is too large: 
* 15854 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/import.cpp#L239) CSV file ends while inside quoted field
* 16329 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/import.cpp#L124) read error, or input line too long (max length: 
* 16808 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/import.cpp#L448) read error: 
* 16809 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/import.cpp#L486) read error: 
* 9998 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/import.cpp#L412) You need to specify fields or have a headerline to 


src/mongo/tools/restore.cpp
----
* 15936 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/restore.cpp#L817) Creating collection " + _curns + " failed. Errmsg: " + info["errmsg
* 16441 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/restore.cpp#L869) Error calling getLastError: " << err["errmsg
* 17370 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/restore.cpp#L100) 
* 17371 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/restore.cpp#L713) 
* 17372 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/restore.cpp#L168) 
* 17373 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/restore.cpp#L180) 
* 17407 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/restore.cpp#L641) 
* 17408 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/restore.cpp#L696) 
* 17410 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/restore.cpp#L144) 
* 17411 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/restore.cpp#L151) 
* 17412 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/restore.cpp#L552) 
* 17413 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/restore.cpp#L571) 
* 17414 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/restore.cpp#L650) 
* 17415 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/restore.cpp#L666) 
* 17416 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/restore.cpp#L627) 


src/mongo/tools/sniffer.cpp
----
* 10266 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/sniffer.cpp#L494) can't use --source twice
* 10267 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/sniffer.cpp#L495) source needs more args


src/mongo/tools/tool.cpp
----
* 10264 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/tool.cpp#L339) invalid object size: 
* 10265 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/tools/tool.cpp#L380) counts don't match


src/mongo/unittest/crutch.cpp
----
* 17249 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/unittest/crutch.cpp#L49) 
* 17250 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/unittest/crutch.cpp#L58) 


src/mongo/unittest/temp_dir.cpp
----
* 17146 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/unittest/temp_dir.cpp#L82) /\\
* 17147 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/unittest/temp_dir.cpp#L93) 


src/mongo/unittest/unittest.cpp
----
* 10162 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/unittest/unittest.cpp#L306) 
* 16145 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/unittest/unittest.cpp#L244) 


src/mongo/util/alignedbuilder.cpp
----
* 13524 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/alignedbuilder.cpp#L122) out of memory AlignedBuilder
* 13584 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/alignedbuilder.cpp#L40) out of memory AlignedBuilder


src/mongo/util/assert_util.h
----
* 10107 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L51) 
* 10437 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L295) unknown exception
* 123 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L91) blah
* 13104 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L48) 
* 13294 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L293) 
* 13297 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L45) 
* 13388 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L46) 
* 13435 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L50) 
* 13436 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L49) 
* 14043 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L304) 
* 14044 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L306) unknown exception
* 16199 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L255) 
* 9996 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/assert_util.h#L47) 


src/mongo/util/background.cpp
----
* 17234 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/background.cpp#L193) 


src/mongo/util/base64.cpp
----
* 10270 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/base64.cpp#L92) invalid base64


src/mongo/util/concurrency/list.h
----
* 14050 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/list.h#L98) List1: item to orphan not in list


src/mongo/util/concurrency/qlock.h
----
* 16137 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L347) 
* 16138 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L353) 
* 16139 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L364) 
* 16140 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L371) 
* 16200 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L144) 
* 16201 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L150) 
* 16202 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L235) 
* 16203 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L246) 
* 16204 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L247) 
* 16205 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L248) 
* 16206 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L266) 
* 16207 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L267) 
* 16208 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L268) 
* 16209 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L279) 
* 16210 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L280) 
* 16211 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L281) 
* 16212 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L291) 
* 16214 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L302) 
* 16215 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L303) 
* 16216 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L312) 
* 16217 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L313) 
* 16219 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L320) 
* 16220 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L321) 
* 16221 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L322) 
* 16222 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/concurrency/qlock.h#L323) 


src/mongo/util/descriptive_stats.h
----
* 16476 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/descriptive_stats.h#L138) the requested value is out of the range of the computed quantiles


src/mongo/util/fail_point.cpp
----
* 16442 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/fail_point.cpp#L68) mode not supported 
* 16443 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/fail_point.cpp#L125) 
* 16444 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/fail_point.cpp#L140) 
* 16445 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/fail_point.cpp#L168) 


src/mongo/util/file.cpp
----
* 10438 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file.cpp#L137) 
* 16569 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file.cpp#L261) 


src/mongo/util/file_allocator.cpp
----
* 10439 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L331) 
* 10440 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L206) 
* 10441 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L210) Unable to allocate new file of size 
* 10442 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L212) Unable to allocate new file of size 
* 10443 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L236) FileAllocator: file write failed
* 13653 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L353) 
* 16062 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L159) fstatfs failed: 
* 16063 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/file_allocator.cpp#L188) ftruncate failed: 


src/mongo/util/intrusive_counter.cpp
----
* 16493 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/intrusive_counter.cpp#L39) Tried to create string longer than 


src/mongo/util/logfile.cpp
----
* 13514 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L267) 
* 13515 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L253) 
* 13516 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L191) couldn't open file " << name << " for writing 
* 13517 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L143) error appending to file 
* 13518 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L87) couldn't open file " << name << " for writing 
* 13519 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L141) error 87 appending to file - invalid parameter
* 15871 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L101) Couldn't truncate file: 
* 15873 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L209) Couldn't truncate file: 
* 16142 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L240) 
* 16143 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L241) 
* 16144 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/logfile.cpp#L239) 


src/mongo/util/mmap.cpp
----
* 13468 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap.cpp#L62) can't create file already exists 
* 13617 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap.cpp#L214) MongoFile : multiple opens of same filename
* 15922 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap.cpp#L86) couldn't get file length when opening mapping 
* 15923 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap.cpp#L96) couldn't get file length when opening mapping 
* 16325 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap.cpp#L47) 
* 16326 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap.cpp#L48) 
* 16327 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap.cpp#L50) 
* 17346 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap.cpp#L245) 


src/mongo/util/mmap_posix.cpp
----
* 10446 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap_posix.cpp#L157) mmap: can't map area of size 0 file: 
* 10447 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap_posix.cpp#L167) map file alloc failed, wanted: " << length << " filelen: 


src/mongo/util/mmap_win.cpp
----
* 13056 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap_win.cpp#L585) Async flushing not supported on windows
* 16148 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap_win.cpp#L482) 
* 16165 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap_win.cpp#L205) 
* 16166 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap_win.cpp#L331) 
* 16167 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap_win.cpp#L444) 
* 16168 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap_win.cpp#L465) 
* 16225 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap_win.cpp#L277) 
* 16362 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap_win.cpp#L401) 
* 16387 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap_win.cpp#L564) 
* 17484 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/mmap_win.cpp#L109) 


src/mongo/util/net/hostandport.h
----
* 13095 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/hostandport.h#L182) HostAndPort: bad port #
* 13110 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/hostandport.h#L178) HostAndPort: host is empty


src/mongo/util/net/httpclient.cpp
----
* 10271 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/httpclient.cpp#L62) invalid url" , url.find( "http://
* 15862 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/httpclient.cpp#L125) no ssl support


src/mongo/util/net/listen.cpp
----
* 15863 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/listen.cpp#L141) listen(): invalid socket? 
* 16723 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/listen.cpp#L424) 
* 16725 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/listen.cpp#L365) 
* 16726 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/listen.cpp#L342) 
* 16727 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/listen.cpp#L410) 
* 16728 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/listen.cpp#L356) 


src/mongo/util/net/message.h
----
* 13273 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/message.h#L190) single data buffer expected
* 16141 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/message.h#L79) cannot translate opcode 


src/mongo/util/net/message_port.cpp
----
* 17132 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/message_port.cpp#L205) 
* 17133 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/message_port.cpp#L200) 
* 17189 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/message_port.cpp#L213) The server is configured to only allow SSL connections


src/mongo/util/net/sock.cpp
----
* 13079 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L167) path to unix socket too long
* 13080 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L165) no unix socket support on windows
* 13082 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L257) getnameinfo error 
* 16501 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L210) 
* 16502 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L225) 
* 16503 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L475) 
* 16506 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L491) 
* 16507 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L607) 
* 16508 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/sock.cpp#L719) 


src/mongo/util/net/socket_poll.cpp
----
* 17185 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/socket_poll.cpp#L56) 


src/mongo/util/net/ssl_manager.cpp
----
* 15861 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/ssl_manager.cpp#L354) Error creating new SSL object 
* 15864 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/ssl_manager.cpp#L533) 
* 16562 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/ssl_manager.cpp#L420) ssl initialization problem
* 16583 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/ssl_manager.cpp#L673) 
* 16584 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/ssl_manager.cpp#L677) 
* 16703 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/ssl_manager.cpp#L522) 
* 16768 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/ssl_manager.cpp#L404) ssl initialization problem
* 16884 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/ssl_manager.cpp#L323) unable to allocate BIO memory
* 16941 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/ssl_manager.cpp#L413) ssl initialization problem
* 16942 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/ssl_manager.cpp#L424) ssl initialization problem
* 16943 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/ssl_manager.cpp#L429) ssl initialization problem
* 16944 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/ssl_manager.cpp#L434) ssl initialization problem
* 17089 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/ssl_manager.cpp#L527) 
* 17314 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/net/ssl_manager.cpp#L455) 


src/mongo/util/ntservice.cpp
----
* 16454 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/ntservice.cpp#L515) 


src/mongo/util/options_parser/value.h
----
* 17114 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/options_parser/value.h#L175) 


src/mongo/util/paths.cpp
----
* 13650 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/paths.cpp#L53) Couldn't open directory '
* 13651 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/paths.cpp#L72) Couldn't fsync directory '
* 13652 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/paths.cpp#L40) Couldn't find parent dir for file: 


src/mongo/util/paths.h
----
* 13646 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/paths.h#L98) stat() failed for file: " << path << " 


src/mongo/util/processinfo_linux2.cpp
----
* 13538 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/processinfo_linux2.cpp#L60) 


src/mongo/util/processinfo_sunos5.cpp
----
* 16846 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/processinfo_sunos5.cpp#L62) 
* 16847 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/processinfo_sunos5.cpp#L69) 
* 16848 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/processinfo_sunos5.cpp#L80) 
* 16849 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/processinfo_sunos5.cpp#L87) 


src/mongo/util/signal_handlers.cpp
----
* 10297 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/signal_handlers.cpp#L339) 
* 16781 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/signal_handlers.cpp#L305) 
* 16782 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/signal_handlers.cpp#L309) 


src/mongo/util/stacktrace_win.cpp
----
* 17006 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/stacktrace_win.cpp#L289) 


src/mongo/util/text.cpp
----
* 13305 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/text.cpp#L152) could not convert string to long long
* 13306 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/text.cpp#L161) could not convert string to long long
* 13307 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/text.cpp#L147) cannot convert empty string to long long
* 13310 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/text.cpp#L165) could not convert string to long long
* 16091 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/text.cpp#L197) 


src/mongo/util/time_support.cpp
----
* 16226 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/time_support.cpp#L126) 
* 16227 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/time_support.cpp#L137) 
* 16228 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/time_support.cpp#L742) 
* 17318 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/time_support.cpp#L345) 


src/mongo/util/timer.cpp
----
* 16160 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/timer.cpp#L98) 
* 16161 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/timer.cpp#L76) 
* 16162 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/timer.cpp#L123) 
* 16163 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/timer.cpp#L124) 


src/mongo/util/unordered_fast_key_table_internal.h
----
* 16471 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/unordered_fast_key_table_internal.h#L168) UnorderedFastKeyTable couldn't add entry after growing many times
* 16845 [code](http://github.com/mongodb/mongo/blob/master/src/mongo/util/unordered_fast_key_table_internal.h#L207) 

