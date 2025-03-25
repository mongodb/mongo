(function() {
    "use strict";
    
    const runTest = function(st, mongos) {
        const adminDB = mongos.getDB("admin");
        const dbName = "test";
        const collName = "test";
    
        adminDB.createUser({user: "admin", pwd: "admin", roles: ["root"]});
        adminDB.auth("admin", "admin");
    
        adminDB.runCommand({enableSharding: dbName});
    
        const testDB = mongos.getDB(dbName);
        testDB.createUser({user: "user", pwd: "user", roles: ["read"]});
    
        adminDB.logout();
        testDB.auth("user", "user");
    
        const lsid = UUID();
        const uid = computeSHA256Block("abc");

        const cmd = {
              aggregate: collName,
              pipeline: [
                {
                    $mergeCursors: {
                        lsid: {id: lsid, uid: uid},
                        remotes: [
                            {
                                shardId: "repro-rs0",
                                hostAndPort: st.rs0.getPrimary().host,
                                cursorResponse: {
                                    cursor: {
                                        id: NumberLong(12345),
                                        ns: `${dbName}.${collName}`,
                                        firstBatch: []
                                    },
                                    ok: 1
                                }
                            },
                            {
                                shardId: "repro-rs1",
                                hostAndPort: st.rs1.getPrimary().host,
                                cursorResponse: {
                                    cursor: {
                                        id: NumberLong(67890),
                                        ns: `${dbName}.${collName}`,
                                        firstBatch: []
                                    },
                                    ok: 1
                                }
                            }
                        ],
                        nss: `${dbName}.${collName}`,
                    }
                },
            ],
            cursor: {},
        };
        assert.commandWorked(testDB.runCommand(cmd));
    };
    
    const st = new ShardingTest({
        keyFile: "jstests/libs/key1", 
        shards: {rs0: {nodes: 2}, rs1: {nodes: 2}},
        mongos: 1
    });

    runTest(st, st.s);
    st.stop();
})();
