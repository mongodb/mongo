// Ensures that sane errors are thrown from mongos v2.6 when present in a cluster with mongod v2.8
// shards
// TODO: Remove post v2.8

load('./jstests/multiVersion/libs/multi_rs.js');
load('./jstests/multiVersion/libs/multi_cluster.js');

// Clearing the connection pool deterministically is awkward - the actual command may fail due to
// using the conn pool in order to talk to the config servers
function clearConnPool(mongos) {
    assert.soon(function() {
        try {
            return mongos.adminCommand({connPoolSync : true}).ok;
        } catch (ex) {
            printjson(ex);
            return false;
        }
    });
}

// Errors are A) currently opaque, and B) wrapped in other error messages, making it awkward to
// detect different codes
function isProtocolError(ex) {
    return /15907/.test(ex.toString());
}

function testBadUpgrade(isRSCluster) {

    jsTest.log("Starting " + (isRSCluster ? "(replica set)" : "") + " cluster...");

    var options = {mongosOptions : {binVersion : "2.6"},
                   configOptions : {binVersion : "2.6"},
                   shardOptions : {binVersion : "2.6"},
                   rsOptions : {binVersion : "2.6", nodes : 2},
                   separateConfig : true,
                   sync : false,
                   rs : isRSCluster};

    var st = new ShardingTest({shards : 2, mongos : 1, other : options});

    var mongos = st.s0;
    var mongosV26 = MongoRunner.runMongos({configdb : st._configDB, binVersion : "2.6"});

    mongos.forceWriteMode("commands");
    mongosV26.forceWriteMode("commands");

    var coll = mongos.getCollection("foo.bar");
    var collV26 = mongosV26.getCollection(coll.toString());

    // Insert a single document to use later
    assert.writeOK(coll.insert({hello : "world"}));

    jsTest.log("Upgrading cluster...");
    // Upgrade config metadata
    // Mongos terminates after upgrading
    assert.eq(null, MongoRunner.runMongos({binVersion : "2.8",
                                           configdb : st._configDB,
                                           upgrade : ''}));
    // Upgrade cluster binaries
    st.upgradeCluster("2.8");
    jsTest.log("Cluster upgraded.");

    // Flush connection pools to avoid transient connection failures
    clearConnPool(mongos);
    clearConnPool(mongosV26);

    // Queries should succeed v2.8, fail v2.6
    assert.neq(null, coll.findOne({}));

    try {
        collV26.findOne({});
        assert(false, "should not be able to query v2.8 shards from v2.6 mongos");
    } catch (ex) {
        assert(isProtocolError(ex), tojson(ex));
    }

    // Writes should succeed v2.8, fail v2.6
    assert.writeOK(coll.insert({hello : "world"}));

    var result = collV26.insert({hello : "world"});
    assert.writeError(result);
    assert(isProtocolError(result.getWriteError().errmsg));

    MongoRunner.stopMongos(mongosV26);
    st.stop();
}

// TODO: SERVER-16321
//testBadUpgrade(false);
//testBadUpgrade(true);
