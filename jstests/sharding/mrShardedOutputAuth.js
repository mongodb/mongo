/**
 * mrShardedOutputAuth.js -- SERVER-7641
 * Test that a mapReduce job can write sharded output to a database
 * from a separate input database while authenticated to both.
 */

(function() {

    function doMapReduce(connection, outputDb) {
        // clean output db and run m/r
        outputDb.numbers_out.drop();
        printjson(connection.getDB('input').runCommand({
            mapreduce: "numbers",
            map: function() {
                emit(this.num, {count: 1});
            },
            reduce: function(k, values) {
                var result = {};
                values.forEach(function(value) {
                    result.count = 1;
                });
                return result;
            },
            out: {merge: "numbers_out", sharded: true, db: "output"},
            verbose: true,
            query: {}
        }));
    }

    function assertSuccess(configDb, outputDb) {
        assert.eq(outputDb.numbers_out.count(), 50, "map/reduce failed");
        assert(!configDb.collections.findOne().dropped, "no sharded collections");
    }

    function assertFailure(configDb, outputDb) {
        assert.eq(outputDb.numbers_out.count(), 0, "map/reduce should not have succeeded");
    }

    var st = new ShardingTest(
        {name: "mrShardedOutputAuth", shards: 1, mongos: 1, other: {keyFile: 'jstests/libs/key1'}});

    // Setup the users to the input, output and admin databases
    var mongos = st.s;
    var adminDb = mongos.getDB("admin");
    adminDb.createUser({user: "user", pwd: "pass", roles: jsTest.adminUserRoles});

    var authenticatedConn = new Mongo(mongos.host);
    authenticatedConn.getDB('admin').auth("user", "pass");
    adminDb = authenticatedConn.getDB("admin");

    var configDb = authenticatedConn.getDB("config");

    var inputDb = authenticatedConn.getDB("input");
    inputDb.createUser({user: "user", pwd: "pass", roles: jsTest.basicUserRoles});

    var outputDb = authenticatedConn.getDB("output");
    outputDb.createUser({user: "user", pwd: "pass", roles: jsTest.basicUserRoles});

    // Setup the input db
    inputDb.numbers.drop();
    for (var i = 0; i < 50; i++) {
        inputDb.numbers.insert({num: i});
    }
    assert.eq(inputDb.numbers.count(), 50);

    // Setup a connection authenticated to both input and output db
    var inputOutputAuthConn = new Mongo(mongos.host);
    inputOutputAuthConn.getDB('input').auth("user", "pass");
    inputOutputAuthConn.getDB('output').auth("user", "pass");
    doMapReduce(inputOutputAuthConn, outputDb);
    assertSuccess(configDb, outputDb);

    // setup a connection authenticated to only input db
    var inputAuthConn = new Mongo(mongos.host);
    inputAuthConn.getDB('input').auth("user", "pass");
    doMapReduce(inputAuthConn, outputDb);
    assertFailure(configDb, outputDb);

    // setup a connection authenticated to only output db
    var outputAuthConn = new Mongo(mongos.host);
    outputAuthConn.getDB('output').auth("user", "pass");
    doMapReduce(outputAuthConn, outputDb);
    assertFailure(configDb, outputDb);

    st.stop();

})();
