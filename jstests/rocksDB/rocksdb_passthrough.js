// XXX only instantiating a ShardingTest because I don't know of anything better
var myTest = new ShardingTest("rocksdb_passthrough", 2, 0, 1);

var db = myTest.getDB("test");
//db.getMongo().forceWriteMode("commands");
//_useWriteCommandsDefault = function() { return true; }; // for tests launching parallel shells.

// XXX ???
//var res = db.adminCommand({setParameter: 1, useClusterWriteCommands: true });

var files = listFiles("jstests/core");

var runnerStart = new Date();

files.forEach(function(x) {
    var mmapSpecificPattern = new RegExp('[\\]\\\\](' +
                //'capped7|' +
                ')\.js$');
    
    if (mmapSpecificPattern.test(x.name)) {
        print( ">>>>>>>>>>>>>>>>>>>>> skipping test that would fail due to mmap-specific test: "
            + x.name);
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
    db = myTest.getDB("test");
});

myTest.stop();

var runnerEnd = new Date();

print("total runner time: " + ((runnerEnd.getTime() - runnerStart.getTime()) / 1000) + "secs");

