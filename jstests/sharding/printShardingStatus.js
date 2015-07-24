// Check that the output from printShardingStatus() (aka sh.status())
// contains important information that it should, like the major section
// headings and the names of sharded collections and their shard keys.


(function () {


// SERVER-19368 to move this into utils.js
// jstests/auth/show_log_auth.js does something similar and could also benefit from this
print.captureAllOutput = function (fn, args) {
    var res = {};
    res.output = [];
    var __orig_print = print;
    print = function () {
        Array.prototype.push.apply(res.output, Array.prototype.slice.call(arguments).join(" ").split("\n"));
    };
    res.result = fn.apply(undefined, args);
    print = __orig_print;
    return res;
}

var st = new ShardingTest({ shards: 1, mongos: 1, config: 1, other: { smallfiles: true } });

var mongos = st.s0;
var admin = mongos.getDB( "admin" );

var dbName = "thisIsTheDatabase";
var collName = "thisIsTheCollection";
var shardKeyName = "thisIsTheShardKey";
var nsName = dbName + "." + collName;

assert.commandWorked( admin.runCommand({ enableSharding: dbName }) );
var key = {};
key[shardKeyName] = 1;
assert.commandWorked( admin.runCommand({ shardCollection: nsName, key: key }) );

var res = print.captureAllOutput( function () { return st.printShardingStatus(); } );
var output = res.output.join("\n");
jsTestLog(output);


function assertPresentInOutput(content, what) {
    assert(output.includes(content), what + " \"" + content + "\" not present in output of printShardingStatus()");
}

assertPresentInOutput("shards:", "section header");
assertPresentInOutput("databases:", "section header");
assertPresentInOutput("balancer:", "section header");

assertPresentInOutput(dbName, "database");
assertPresentInOutput(collName, "collection");
assertPresentInOutput(shardKeyName, "shard key");

st.stop();

})();
