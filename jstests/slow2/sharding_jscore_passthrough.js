var db;

(function() {
     "use strict"

     var myShardingTest = new ShardingTest("sharding_passthrough", 1, 0, 1);
     myShardingTest.adminCommand({ enablesharding : "test" });

     db = myShardingTest.getDB("test");
     db.getMongo().forceWriteMode("commands");
     _useWriteCommandsDefault = function() { return true; }; // for tests launching parallel shells.

     var res = db.adminCommand({ setParameter: 1, useClusterWriteCommands: true });
     var files = listFiles("jstests/core");

     var runnerStart = new Date();

     files.forEach(
         function(x) {
             if (/[\/\\]_/.test(x.name) || ! /\.js$/.test(x.name)) {
                 print(" >>>>>>>>>>>>>>> skipping " + x.name);
                 return;
             }

             // Notes:

             // apply_ops*: mongos doesn't implement "applyOps" -- SERVER-1439.

             // copydb, copydb2: copyDatabase seems not to work at all in
             //                  the ShardingTest setup.  SERVER-1440.

             // dbcase: Database names are case-insensitive under ShardingTest?
             //         SERVER-1443.

             // These are all SERVER-1444
             // count5: limit() and maybe skip() may be unreliable.
             // geo3: limit() not working, I think.
             // or4: skip() not working?

             // shellkillop: dunno yet.  SERVER-1445

             // update_setOnInsert: db.setPrifilingLevel is not working. SERVER-8653

             // These should simply not be run under sharding:
             // dbadmin: Uncertain  Cut-n-pasting its contents into mongo worked.
             // error1: getpreverror not supported under sharding.
             // fsync, fsync2: isn't supported through mongos.
             // remove5: getpreverror, I think. don't run.
             // update4: getpreverror don't run.

             // Around July 20, command passthrough went away, and these
             // commands weren't implemented:
             // clean cloneCollectionAsCapped copydbgetnonce dataSize
             // datasize dbstats deleteIndexes dropIndexes forceerror
             // getnonce logout profile reIndex repairDatabase
             // reseterror splitVector validate top

             /* missing commands :
              * forceerror and switchtoclienterrors
              * cloneCollectionAsCapped
              * splitvector
              * profile (apitest_db, cursor6, evalb)
              * copydbgetnonce
              * dbhash
              * medianKey
              * logout and getnonce
              */

             var failsInShardingPattern = new RegExp('[\\/\\\\](' +
                                                     'error3|' +
                                                     'capped.*|' +
                                                     'apitest_db|' +
                                                     'cursor6|' +
                                                     'copydb-auth|' +
                                                     'profile\\d*|' +
                                                     'dbhash|' +
                                                     'dbhash2|' +
                                                     'explain_missing_database|' +
                                                     'median|' +
                                                     'evalb|' +
                                                     'evald|' +
                                                     'eval_nolock|' +
                                                     'auth1|' +
                                                     'auth2|' +
                                                     'dropdb_race|' +
                                                     'unix_socket\\d*|' +
                                                     // TODO: SERVER-17284 remove once find cmd is
                                                     // implemented in mongos
                                                     'find_getmore_bsonsize|' +
                                                     'find_getmore_cmd|' +
                                                     'read_after_optime' +
                                                     ')\.js$');

             // These are bugs (some might be fixed now):
             var mightBeFixedPattern = new RegExp('[\\/\\\\](' +
                                                  'count5|' +
                                                  'or4|' +
                                                  'shellkillop|' +
                                                  'update4|' +
                                                  'update_setOnInsert|' +
                                                  'profile\\d*|' +
                                                  'max_time_ms|' + // Will be fixed when SERVER-2212 is resolved.
                                                  'fts_querylang|' + // Will be fixed when SERVER-9063 is resolved.
                                                  'fts_projection' +
                                                  ')\.js$');

             // These aren't supposed to get run under sharding:
             var notForShardingPattern = new RegExp('[\\/\\\\](' +
                                                    'apply_ops.*|' + // mongos has no applyOps cmd
                                                    'dbadmin|' +
                                                    'error1|' +
                                                    'fsync|' +
                                                    'fsync2|' +
                                                    'geo.*|' +
                                                    'indexh|' +
                                                    'index_bigkeys_nofail|' +
                                                    'remove5|' +
                                                    'update4|' +
                                                    'loglong|' +
                                                    'logpath|' +
                                                    'notablescan|' +
                                                    'collection_truncate|' + // relies on emptycapped test command which isn't in mongos
                                                    'compact.*|' +
                                                    'check_shard_index|' +
                                                    'bench_test.*|' +
                                                    'mr_replaceIntoDB|' +
                                                    'mr_auth|' +
                                                    'queryoptimizera|' +
                                                    'storageDetailsCommand|' +
                                                    'reversecursor|' +
                                                    'stages.*|' +
                                                    'top|' +
                                                    'repair_cursor1|' +
                                                    'touch1|' +
                                                    'query_oplogreplay|' + // no local db on mongos
                                                    'dbcase|' + // undo after fixing SERVER-11735
                                                    'dbcase2|' + // undo after fixing SERVER-11735
                                                    'stats' + // tests db.stats().dataFileVersion, which doesn't appear in sharded db.stats()
                                                    ')\.js$');

             if (failsInShardingPattern.test(x.name)) {
                 print(" >>>>>>>>>>>>>>> skipping test that would correctly fail under sharding: " + x.name);
                 return;
             }

             if (mightBeFixedPattern.test(x.name)) {
                 print(" !!!!!!!!!!!!!!! skipping test that has failed under sharding: " +
                       "but might not anymore " + x.name);
                 return;
             }

             if (notForShardingPattern.test(x.name)) {
                 print(" !!!!!!!!!!!!!!! skipping test that should not run under sharding: " + x.name);
                 return;
             }

             print(" *******************************************");
             print("         Test : " + x.name + " ...");
             print("                " +
                   Date.timeFunc(function() {
                                     load(x.name);
                                 }, 1) + "ms");

             gc(); // TODO SERVER-8683: remove gc() calls once resolved

             // Reset "db" variable, just in case someone broke the rules and used it themselves
             db = myShardingTest.getDB("test");
         });


     myShardingTest.stop();

     var runnerEnd = new Date();

     print("total runner time: " + ((runnerEnd.getTime() - runnerStart.getTime()) / 1000) + "secs");

}());
