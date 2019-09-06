/*
 * Tests that SNI names are advertised if and only if they are a URL, and NOT an IP address.
 */

(function() {
'use strict';

let path = "jstests/libs/";
let pemKeyFile = path + "server.pem";
let caFile = path + "ca.pem";
let testURL = "local.10gen.cc";
let testIP = "127.0.0.1";

let params = {
    tlsCertificateKeyFile: pemKeyFile,
    tlsCAFile: caFile,
    tlsMode: "preferTLS",
    bind_ip: testURL,
    tlsAllowInvalidHostnames: ""
};

/* we will have two test server configurations: one that is bound to a URL, and one that is bound to
 * an IP address
 * The bind_ip here is only to confirm that mongod and the shell are on the same page. bind_ip is
 * not what is used for testing SNI advertisement. That is the IP address supplied to the shell. */
let ipParams = Object.merge(params, {bind_ip: testIP});
let urlParams = params;

// returns the result of command "whatsmysni" from a regular mongod
function getSNI(params) {
    let mongod = MongoRunner.runMongod(params);
    let m = new Mongo(params.bind_ip + ":" + mongod.port);
    let db = m.getDB("admin");

    const sni = assert.commandWorked(db.runCommand({whatsmysni: 1}))["sni"];
    MongoRunner.stopMongod(mongod);

    return sni;
}

// returns the result of command "whatsmysni" performed between nodes of a sharded cluster
function getSNISharded(params) {
    let s = new ShardingTest({
        name: "shard",
        shards: 2,
        useHostname: true,
        host: params.bind_ip,
        other: {configOptions: params, mongosOptions: params, shardOptions: params}
    });
    let db = s.getDB("admin");

    // sort of have to fish out the value from deep within the output of multicast...
    const multicastData =
        assert.commandWorked(db.runCommand({multicast: {whatsmysni: 1}}))["hosts"];
    const hostName = Object.keys(multicastData)[0];
    const sni = multicastData[hostName]["data"]["sni"];

    s.stop();

    return sni;
}

// TODO SERVER-41045 remove if-statement once SNI is supported on Windows
if (!_isWindows()) {
    jsTestLog("Testing mongod bound to URL " + testURL);
    assert.eq(testURL, getSNI(urlParams), "URL host is not advertised as SNI name in basic mongod");
    assert.eq(testURL,
              getSNISharded(urlParams),
              "URL host is not advertised as SNI name in sharded mongod");

    jsTestLog("Testing mongod bound to IP " + testIP);
    assert.eq(false, getSNI(ipParams), "IP host is advertised as SNI name in basic mongod");
    assert.eq(
        false, getSNISharded(ipParams), "IP host is advertised as SNI name in sharded mongod");
}
})();