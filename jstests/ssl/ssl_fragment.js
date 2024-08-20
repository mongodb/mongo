/**
 * Test that a large request and response works correctly.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runTest(conn) {
    // SSL packets have a max size of ~16 kb so to test packet fragmentation support, create a
    // string larger then 16kb.
    const chunk = 'E$%G^56w4v5v54Vv$V@#t2#%t56u7B$ub%6 NU@ Y3qv4Yq%yq4C%yx$%zh';  // random data
    let s = '';
    while (s.length < (8 * 1024 * 1024)) {
        s += chunk;
    }

    const ssl_frag = conn.getCollection('test.ssl_frag');
    assert.commandWorked(ssl_frag.insert({_id: "large_str", foo: s}));

    const read = ssl_frag.find({_id: "large_str"}).toArray()[0].foo;
    assert.eq(s, read, "Did not receive value written");
}

let options = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: "jstests/libs/server.pem",
    networkMessageCompressors: 'disabled',
    tlsCAFile: 'jstests/libs/ca.pem',
    tlsAllowInvalidCertificates: ""
};

let mongosOptions = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: "jstests/libs/server.pem",
    networkMessageCompressors: 'disabled',
    tlsCAFile: 'jstests/libs/ca.pem',
    tlsAllowInvalidCertificates: ""
};

if (_isWindows()) {
    // Force the ASIO stack to do small reads which will excerise the schannel buffering code
    // and significantly slow down the test
    options =
        Object.extend(options, {setParameter: {"failpoint.smallTLSReads": "{'mode':'alwaysOn'}"}});
    mongosOptions = Object.extend(
        mongosOptions, {setParameter: {"failpoint.smallTLSReads": "{'mode':'alwaysOn'}"}});
}

const mongod = MongoRunner.runMongod(options);
runTest(mongod);
MongoRunner.stopMongod(mongod);

const st = new ShardingTest({
    shards: 3,
    mongos: 1,
    config: 1,
    other: {configOptions: options, mongosOptions: mongosOptions, shardOptions: options}
});

runTest(st.s0);
st.stop();