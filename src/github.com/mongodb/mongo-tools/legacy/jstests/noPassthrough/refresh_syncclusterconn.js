/* SERVER-4385
 * SyncClusterConnection should refresh sub-connections on recieving exceptions
 *
 * 1. Start 3 config servers.
 * 2. Create a syncclusterconnection to the servers from step 1.
 * 3. Restart one of the config servers.
 * 4. Try an insert. It should fail. This will also refresh the sub connection.
 * 5. Try an insert again. This should work fine.
 */

var mongoA = MongoRunner.runMongod({});
var mongoB = MongoRunner.runMongod({});
var mongoC = MongoRunner.runMongod({});
var mongoSCC = new Mongo(mongoA.host + "," + mongoB.host + "," + mongoC.host);

MongoRunner.stopMongod(mongoA);
MongoRunner.runMongod({ restart: mongoA.runId });

try {
    mongoSCC.getCollection("foo.bar").insert({ x : 1});
    assert(false , "must throw an insert exception");
} catch (e) {
    printjson(e);
}

mongoSCC.getCollection("foo.bar").insert({ blah : "blah" });
assert.eq(null, mongoSCC.getDB("foo").getLastError());
