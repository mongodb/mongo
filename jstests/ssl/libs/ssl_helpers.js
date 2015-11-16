load('jstests/multiVersion/libs/multi_rs.js');

//=== Shared SSL testing library functions and constants ===

var KEYFILE = "jstests/libs/key1";
var SERVER_CERT = "jstests/libs/server.pem";
var CA_CERT = "jstests/libs/ca.pem";
var CLIENT_CERT = "jstests/libs/client.pem";

// Note: "sslAllowInvalidCertificates" is enabled to avoid
// hostname conflicts with our testing certificates
var disabled = {sslMode: "disabled"};
var allowSSL = {sslMode : "allowSSL",
    sslAllowInvalidCertificates: "",
    sslPEMKeyFile : SERVER_CERT,
    sslCAFile: CA_CERT};
var preferSSL = {sslMode : "preferSSL",
    sslAllowInvalidCertificates: "",
    sslPEMKeyFile :  SERVER_CERT,
    sslCAFile: CA_CERT};
var requireSSL = {sslMode : "requireSSL",
    sslAllowInvalidCertificates: "",
    sslPEMKeyFile : SERVER_CERT,
    sslCAFile: CA_CERT};

// Test if ssl replset  configs work

var replSetTestFile = "jstests/replsets/replset1.js";

var replShouldSucceed = function(name, opt1, opt2) {
    ssl_options1 = opt1;
    ssl_options2 = opt2;
    ssl_name = name;
    // try running this file using the given config
    load(replSetTestFile);
};

// Test if ssl replset configs fail
var replShouldFail = function(name, opt1, opt2) {
    ssl_options1 = opt1;
    ssl_options2 = opt2;
    ssl_name = name;
    replTest = null;
    assert.throws(load,[replSetTestFile],
                  "This setup should have failed");
    // clean up to continue running...
    if (replTest) {
        replTest.stopSet(15);
    }
};

/**
 * Takes in two mongod/mongos configuration options and runs a basic
 * sharding test to see if they can work together...
 */
function mixedShardTest(options1, options2, shouldSucceed) {
    try {
        var st = new ShardingTest({
            mongos : [options1],
            config : [options1],
            shards : [options1, options2]
        });
        st.stopBalancer();

        // Test mongos talking to config servers
        var r = st.adminCommand({enableSharding: "test"});
        assert.eq(r, true, "error enabling sharding for this configuration");

        st.ensurePrimaryShard("test", "shard0000");
        r = st.adminCommand({ movePrimary: 'test', to: 'shard0001' });

        var db1 = st.getDB("test");
        r = st.adminCommand({ shardCollection : "test.col" , key : { _id : 1 } });
        assert.eq(r, true, "error sharding collection for this configuration");

        // Test mongos talking to shards
        var bigstr = Array(1024*1024).join("#");

        var bulk = db1.col.initializeUnorderedBulkOp();
        for(var i = 0; i < 128; i++){
            bulk.insert({ _id: i, string: bigstr });
        }
        assert.writeOK(bulk.execute());
        assert.eq(128, db1.col.count(), "error retrieving documents from cluster");

        // Test shards talking to each other
        r = st.getDB('test').adminCommand({ moveChunk: 'test.col',
                                            find: { _id: 0 }, to: 'shard0000' });
        assert(r.ok, "error moving chunks: " + tojson(r));

        db1.col.remove({});

    } catch(e) {
        if (shouldSucceed) throw e;
        //silence error if we should fail...
        print("IMPORTANT! => Test failed when it should have failed...continuing...");
    } finally {
        // This has to be done in order for failure
        // to not prevent future tests from running...
        if(st) {
            st.stop();
        }
    }
}

