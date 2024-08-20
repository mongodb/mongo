// Tests basic sharding with x509 cluster auth. The purpose is to verify the connectivity between
// mongos and the shards.
// @tags: [
//   disables_test_commands,
// ]
import {findMatchingLogLine} from "jstests/libs/log.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const x509_options = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: "jstests/libs/server.pem",
    tlsCAFile: "jstests/libs/ca.pem",
    tlsClusterFile: "jstests/libs/cluster_cert.pem",
    tlsAllowInvalidHostnames: "",
    clusterAuthMode: "x509"
};

function runTest() {
    // Start ShardingTest with enableBalancer because ShardingTest attempts to turn off the balancer
    // otherwise, which it will not be authorized to do. Once SERVER-14017 is fixed the
    // "enableBalancer" line could be removed.
    const st = new ShardingTest({
        shards: 2,
        mongos: 1,
        other: {
            enableBalancer: true,
            configOptions: x509_options,
            mongosOptions: x509_options,
            rsOptions: x509_options,
            shardOptions: x509_options
        }
    });

    st.s.getDB('admin').createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
    st.s.getDB('admin').auth('admin', 'pwd');

    const coll = st.s.getCollection("test.foo");

    st.shardColl(coll, {insert: 1}, false);

    // Authenticate the config server and verify that a log line concerning a username change does
    // not appear on the config server since we are doing intracluster auth using X509.
    st.c0.getDB('admin').auth('admin', 'pwd');
    const globalLog = assert.commandWorked(st.c0.adminCommand({getLog: "global"}));
    const fieldMatcher = {msg: "Different user name was supplied to saslSupportedMechs"};
    assert.eq(
        null,
        findMatchingLogLine(globalLog.log, fieldMatcher),
        "Found log line concerning \"Different user name was supplied to saslSupportedMechs\" when we did not expect to.");

    print("starting insertion phase");

    // Insert a bunch of data
    const toInsert = 2000;
    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < toInsert; i++) {
        bulk.insert({my: "test", data: "to", insert: i});
    }
    assert.commandWorked(bulk.execute());

    print("starting updating phase");

    // Update a bunch of data
    const toUpdate = toInsert;
    bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < toUpdate; i++) {
        const id = coll.findOne({insert: i})._id;
        bulk.find({insert: i, _id: id}).update({$inc: {counter: 1}});
    }
    assert.commandWorked(bulk.execute());

    print("starting deletion");

    // Remove a bunch of data
    const toDelete = toInsert / 2;
    bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < toDelete; i++) {
        bulk.find({insert: i}).removeOne();
    }
    assert.commandWorked(bulk.execute());

    // Make sure the right amount of data is there
    assert.eq(coll.find().itcount({my: 'test'}), toInsert / 2);

    // Authenticate csrs so ReplSetTest.stopSet() can do db hash check.
    if (st.configRS) {
        st.configRS.nodes.forEach((node) => {
            node.getDB('admin').auth('admin', 'pwd');
        });
    }

    st.stop();
}

TestData.enableTestCommands = true;
runTest();
TestData.enableTestCommands = false;
runTest();
