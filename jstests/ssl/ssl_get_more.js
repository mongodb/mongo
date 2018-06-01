(function() {
    "use strict";

    var x509_options = {
        sslMode: "requireSSL",
        sslPEMKeyFile: "jstests/libs/server.pem",
        sslCAFile: "jstests/libs/ca.pem",
        sslClusterFile: "jstests/libs/cluster_cert.pem",
        sslAllowInvalidHostnames: "",
        clusterAuthMode: "x509"
    };

    const st = new ShardingTest({
        shards: 1,
        other: {
            enableBalancer: true,
            configOptions: x509_options,
            mongosOptions: x509_options,
            rsOptions: x509_options,
            shardOptions: x509_options,
            shardAsReplicaSet: false
        }
    });

    st.s.getDB('admin').createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
    st.s.getDB('admin').auth('admin', 'pwd');

    const sessionOptions = {causalConsistency: false};
    const session = st.s.startSession(sessionOptions);
    const db = session.getDatabase("test");
    const coll = db.foo;

    coll.createIndex({x: 1});
    coll.createIndex({y: 1});

    for (let i = 0; i < 10; i++) {
        const res = assert.commandWorked(
            db.runCommand({listIndexes: coll.getName(), cursor: {batchSize: 0}}));
        const cursor = new DBCommandCursor(db, res);
        assert.eq(3, cursor.itcount());
    }

    // Authenticate csrs so ReplSetTest.stopSet() can do db hash check.
    if (st.configRS) {
        st.configRS.nodes.forEach((node) => {
            node.getDB('admin').auth('admin', 'pwd');
        });
    }
    st.stop();
}());
