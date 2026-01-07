export var makeNewConnWithExistingSession = function (connStr, session) {
    // We may fail to connect if the continuous stepdown thread had just terminated
    // or killed a primary. We therefore use the connect() function defined in
    // network_error_and_txn_override.js to add automatic retries to
    // connections. The override is loaded in worker_thread.js.
    const conn = connect(connStr).getMongo();
    conn._defaultSession = new _DelegatingDriverSession(conn, session);
    return conn;
};

export var getReplSetName = function (conn) {
    let res;
    assert.soonNoExcept(
        () => {
            res = conn.getDB("admin").runCommand({isMaster: 1});
            return true;
        },
        "Failed to establish a connection to the replica set",
        undefined, // default timeout is 10 mins
        2 * 1000,
    ); // retry on a 2 second interval

    assert.commandWorked(res);
    assert.eq("string", typeof res.setName, () => `not connected to a replica set: ${tojson(res)}`);
    return res.setName;
};

export var makeReplSetConnWithExistingSession = function (connStrList, replSetName, tid, session) {
    let connStr = `mongodb://${connStrList.join(",")}/?appName=tid:${tid}&replicaSet=${replSetName}`;
    if (jsTestOptions().shellGRPC) {
        connStr += "&grpc=false";
    }
    return makeNewConnWithExistingSession(connStr, session);
};
