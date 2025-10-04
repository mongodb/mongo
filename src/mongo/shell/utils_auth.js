let authutil;

assert(!authutil);
authutil = {};

/**
 * Logs out all connections "conn" from database "dbname".
 */
authutil.logout = function (conn, dbname) {
    let i;
    if (null == conn.length) {
        conn = [conn];
    }
    for (i = 0; i < conn.length; ++i) {
        let curDB = new DB(conn[i], dbname);
        curDB.logout();
    }
};

/**
 * Authenticates all connections in "conns" using "authParams" on database "dbName".
 *
 * Raises an exception if any authentication fails, and tries to leave all connnections
 * in "conns" in the logged-out-of-dbName state.
 */
authutil.assertAuthenticate = function (conns, dbName, authParams) {
    if (conns.length == null) conns = [conns];

    try {
        for (let i = 0; i < conns.length; ++i) {
            let conn = conns[i];
            // Bypass the implicit auth call in getDB();
            const db = new DB(conn, dbName);
            try {
                retryOnNetworkError(db._authOrThrow.bind(db, authParams));
            } catch (ex3) {
                doassert(
                    "assert failed : " +
                        "Failed to authenticate " +
                        conn +
                        " to " +
                        dbName +
                        " using parameters " +
                        tojson(authParams) +
                        " : " +
                        ex3,
                );
            }
        }
    } catch (ex) {
        try {
            authutil.logout(conns, dbName);
        } catch (ex2) {}
        throw ex;
    }
};

/**
 * Authenticates all connections in "conns" using "authParams" on database "dbName".
 * Raises in exception if any of the authentications succeed.
 */
authutil.assertAuthenticateFails = function (conns, dbName, authParams) {
    let conn, i;
    if (conns.length == null) conns = [conns];

    for (i = 0; i < conns.length; ++i) {
        conn = conns[i];
        // Bypass the implicit auth call in getDB();
        const db = new DB(conn, dbName);
        const ex = assert.throws(
            retryOnNetworkError,
            [db._authOrThrow.bind(db, authParams)],
            "Unexpectedly authenticated " + conn + " to " + dbName + " using parameters " + tojson(authParams),
        );
        if (isNetworkError(ex)) {
            throw ex;
        }
    }
};

/**
 * Executes action() after authenticating the keyfile user on "conn", then logs out the keyfile
 * user.
 */
authutil.asCluster = function (conn, keyfile, action) {
    // put a connection in an array for uniform processing.
    let connArray = conn;
    if (conn.length == null) connArray = [conn];

    const connOptions = connArray[0].fullOptions || {};
    const authMode = connOptions.clusterAuthMode || connArray[0].clusterAuthMode || jsTest.options().clusterAuthMode;

    let clusterTimes = connArray.map((connElem) => {
        const connClusterTime = connElem.getClusterTime();
        const sessionClusterTime = connElem._getDefaultSession().getClusterTime();
        const operationTime = connElem._getDefaultSession().getOperationTime();

        connElem.resetClusterTime_forTesting();
        connElem._getDefaultSession().resetClusterTime_forTesting();
        connElem._getDefaultSession().resetOperationTime_forTesting();

        return {connClusterTime, sessionClusterTime, operationTime};
    });

    let authDB = "admin";
    if (authMode === "keyFile" || authMode === "sendKeyFile" || (authMode === "sendX509" && keyfile !== undefined)) {
        if (keyfile === undefined) {
            keyfile = connOptions.keyFile || connArray[0].keyFile;
            assert(
                keyfile !== undefined,
                `Cannot find a keyfile to use for authentication from the connection: ${tojson(
                    connOptions,
                )} ==== ${tojson(connArray[0])}`,
            );
        }

        authutil.assertAuthenticate(conn, "admin", {
            user: "__system",
            mechanism: "SCRAM-SHA-256",
            pwd: cat(keyfile).replace(/[\011-\015\040]/g, ""),
        });
    } else if (authMode === "x509" || authMode === "sendX509") {
        // When we login as __system, it gets registered internally as __system@local as the user
        // name
        authDB = "local";
        authutil.assertAuthenticate(conn, "$external", {
            mechanism: "MONGODB-X509",
        });
    } else {
        throw new Error("clusterAuthMode " + authMode + " is currently unsupported");
    }

    try {
        return action();
    } finally {
        try {
            authutil.logout(conn, authDB);
            let connArray = conn;
            if (conn.length == null) connArray = [conn];

            for (let i = 0; i < connArray.length; i++) {
                let connElem = connArray[i];
                connElem.resetClusterTime_forTesting();
                connElem._getDefaultSession().resetClusterTime_forTesting();
                connElem._getDefaultSession().resetOperationTime_forTesting();
                if (clusterTimes[i].connClusterTime) {
                    connElem.advanceClusterTime(clusterTimes[i].connClusterTime);
                }
                if (clusterTimes[i].sessionClusterTime) {
                    connElem._getDefaultSession().advanceClusterTime(clusterTimes[i].sessionClusterTime);
                }
                if (clusterTimes[i].operationTime) {
                    connElem._getDefaultSession().advanceOperationTime(clusterTimes[i].operationTime);
                }
            }
        } catch (ex) {}
    }
};

export {authutil};
