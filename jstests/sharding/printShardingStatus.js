// Check that the output from printShardingStatus() (aka sh.status())
// contains important information that it should, like the major section
// headings and the names of sharded collections and their shard keys.


(function () {


var st = new ShardingTest({ shards: 1, mongos: 1, config: 1, other: { smallfiles: true } });

var mongos = st.s0;
var admin = mongos.getDB( "admin" );


function grabStatusOutput(st) {
    var res = print.captureAllOutput( function () { return st.printShardingStatus(); } );
    var output = res.output.join("\n");
    jsTestLog(output);
    return output;
}

function assertPresentInOutput(output, content, what) {
    assert(output.includes(content), what + " \"" + content + "\" NOT present in output of "
                                     + "printShardingStatus() (but it should be)");
}

function assertNotPresentInOutput(output, content, what) {
    assert( ! output.includes(content), what + " \"" + content + "\" IS present in output of "
                                        + "printShardingStatus() (but it should not be)");
}



////////////////////////
// Basic tests
////////////////////////

var dbName = "thisIsTheDatabase";
var collName = "thisIsTheCollection";
var shardKeyName = "thisIsTheShardKey";
var nsName = dbName + "." + collName;

assert.commandWorked( admin.runCommand({ enableSharding: dbName }) );
var key = {};
key[shardKeyName] = 1;
assert.commandWorked( admin.runCommand({ shardCollection: nsName, key: key }) );

var output = grabStatusOutput(st);

assertPresentInOutput(output, "shards:", "section header");
assertPresentInOutput(output, "databases:", "section header");
assertPresentInOutput(output, "balancer:", "section header");

assertPresentInOutput(output, dbName, "database");
assertPresentInOutput(output, collName, "collection");
assertPresentInOutput(output, shardKeyName, "shard key");

assert( mongos.getDB(dbName).dropDatabase() );



////////////////////////
// Extended tests
////////////////////////

var testCollDetailsNum = 0;
function testCollDetails(args) {
    if (args === undefined || typeof(args) != "object") {
        args = {};
    }

    var getCollName = function (x) { return "test.test" + x.zeroPad(4); };
    var collName = getCollName(testCollDetailsNum);

    var cmdObj = { shardCollection: collName, key: { _id: 1 } };
    if (args.unique) {
        cmdObj.unique = true;
    }
    assert.commandWorked( admin.runCommand(cmdObj) );

    if (args.hasOwnProperty("unique")) {
        assert.writeOK( mongos.getDB("config").collections.update({ _id : collName },
                { $set : { "unique" : args.unique } }) );
    }
    if (args.hasOwnProperty("noBalance")) {
        assert.writeOK( mongos.getDB("config").collections.update({ _id : collName },
                { $set : { "noBalance" : args.noBalance } }) );
    }

    var output = grabStatusOutput(st);

    assertPresentInOutput(output, collName, "collection");
    // If any of the previous collection names are present, then their optional indicators
    // might also be present.  This might taint the results when we go searching through
    // the output.
    // This also means that earlier collNames can't be a prefix of later collNames.
    for (var i = 0; i < testCollDetailsNum; i++) {
        assertNotPresentInOutput(output, getCollName(i), "previous collection");
    }

    assertPresentInOutput(output, "unique: " + (!!args.unique), "unique shard key indicator");
    if (args.hasOwnProperty("unique") && typeof(args.unique) != "boolean") {
        // non-bool: actual value must be shown
        assertPresentInOutput(output, tojson(args.unique), "unique shard key indicator (non bool)");
    }

    assertPresentInOutput(output,
                          "balancing: " + (!args.noBalance),
                          "balancing indicator (inverse of noBalance)");
    if (args.hasOwnProperty("noBalance") && typeof(args.noBalance) != "boolean") {
        // non-bool: actual value must be shown
        assertPresentInOutput(output, tojson(args.noBalance), "noBalance indicator (non bool)");
    }

    assert( mongos.getCollection(collName).drop() );

    testCollDetailsNum++;
}

assert.commandWorked( admin.runCommand({ enableSharding: "test" }) );

// Defaults
testCollDetails({ });

// Expected values
testCollDetails({ unique: false, noBalance: false });
testCollDetails({ unique: true,  noBalance: true  });

// Unexpected truthy values
testCollDetails({ unique: "truthy unique value 1", noBalance: "truthy noBalance value 1" });
testCollDetails({ unique: 1,                       noBalance: 1 });
testCollDetails({ unique: -1,                      noBalance: -1 });
testCollDetails({ unique: {},                      noBalance: {} });

// Unexpected falsy values
testCollDetails({ unique: "",                      noBalance: "" });
testCollDetails({ unique: 0,                       noBalance: 0 });

assert( mongos.getDB("test").dropDatabase() );



st.stop();

})();
