import {ShardingTest} from "jstests/libs/shardingtest.js";
import {CLUSTER_CERT, requireTLS} from "jstests/ssl/libs/ssl_helpers.js";

const x509_options = Object.extend(requireTLS, {tlsClusterFile: CLUSTER_CERT, clusterAuthMode: "x509"});

const st = new ShardingTest({
    shards: 1,
    other: {
        enableBalancer: true,
        configOptions: x509_options,
        mongosOptions: x509_options,
        rsOptions: x509_options,
    },
});

st.s.getDB("admin").createUser({user: "admin", pwd: "pwd", roles: ["root"]});
st.s.getDB("admin").auth("admin", "pwd");

const sessionOptions = {
    causalConsistency: false,
};
const session = st.s.startSession(sessionOptions);
const db = session.getDatabase("test");
const coll = db.foo;

coll.createIndex({x: 1});
coll.createIndex({y: 1});

for (let i = 0; i < 10; i++) {
    const res = assert.commandWorked(db.runCommand({listIndexes: coll.getName(), cursor: {batchSize: 0}}));
    const cursor = new DBCommandCursor(db, res);
    assert.eq(3, cursor.itcount());
}

assert.commandWorked(db.createCollection("bar"));
assert.commandWorked(db.createCollection("baz"));

for (let i = 0; i < 10; i++) {
    const res = assert.commandWorked(db.runCommand({listCollections: 1, cursor: {batchSize: 0}}));
    const cursor = new DBCommandCursor(db, res);
    assert.eq(3, cursor.itcount());
}

// Authenticate csrs so ReplSetTest.stopSet() can do db hash check.
if (st.configRS) {
    st.configRS.nodes.forEach((node) => {
        node.getDB("admin").auth("admin", "pwd");
    });
}

// Index consistency check during shutdown needs a privileged user to auth as.
const x509User = "CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US";
st.s.getDB("$external").createUser({user: x509User, roles: [{role: "__system", db: "admin"}]});

st.stop();
