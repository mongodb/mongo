// Test SERVER-14306.  Do a query directly against a mongod with an in-memory sort and a limit that
// doesn't cause the in-memory sort limit to be reached, then make sure the same limit also doesn't
// cause the in-memory sort limit to be reached when running through a mongos.
(function() {
     "use strict";

     var st = new ShardingTest({ shards: 2, chunkSize: 1, other: {separateConfig: true}});
     var db = st.s.getDB('test');
     var mongosCol = db.getCollection('skip');
     db.adminCommand({ enableSharding: 'test' });
     db.adminCommand({ shardCollection: 'test.skip', key: { _id: 1 }});
     // Disable balancing of this collection
     assert.writeOK(db.getSiblingDB('config').collections.update({_id: 'test.skip'},
                                                                 {$set: {noBalance: true}}));


     var filler = new Array(10000).toString();
     var bulk = [];
     // create enough data to exceed 32MB in-memory sort limit.
     for (var i = 0; i < 20000; i++) {
         bulk.push({x:i, str:filler});
     }
     assert.writeOK(mongosCol.insert(bulk));
     // Make sure that at least 1 doc is on another shard so that mongos doesn't treat this as a
     // single-shard query (which doesn't exercise the bug)
     assert.commandWorked(db.getSiblingDB('admin').runCommand({moveChunk: 'test.skip',
                                                               find: {_id:1},
                                                               to: 'shard0001'}));

     var docCount = mongosCol.count();
     var shardCol = st.shard0.getDB('test').getCollection('skip');
     var passLimit = 2000;
     var failLimit = 4000;
     jsTestLog("Test no error with limit of " + passLimit + " on mongod");
     assert.eq(passLimit, shardCol.find().sort({x:1}).limit(passLimit).itcount());

     jsTestLog("Test error with limit of " + failLimit + " on mongod");
     assert.throws( function() {shardCol.find().sort({x:1}).limit(failLimit).itcount(); } );

     jsTestLog("Test no error with limit of " + passLimit + " on mongos");
     assert.eq(passLimit, mongosCol.find().sort({x:1}).limit(passLimit).itcount());

     jsTestLog("Test error with limit of " + failLimit + " on mongos");
     assert.throws( function() {mongosCol.find().sort({x:1}).limit(failLimit).itcount(); } );
 })();