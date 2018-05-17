var authutil;

(function() {
    assert(!authutil);
    authutil = {};

    /**
     * Logs out all connections "conn" from database "dbname".
     */
    authutil.logout = function(conn, dbname) {
        var i;
        if (null == conn.length) {
            conn = [conn];
        }
        for (i = 0; i < conn.length; ++i) {
            var curDB = new DB(conn[i], dbname);
            curDB.logout();
        }
    };

    /**
     * Authenticates all connections in "conns" using "authParams" on database "dbName".
     *
     * Raises an exception if any authentication fails, and tries to leave all connnections
     * in "conns" in the logged-out-of-dbName state.
     */
    authutil.assertAuthenticate = function(conns, dbName, authParams) {
        var conn, i, ex, ex2;
        if (conns.length == null)
            conns = [conns];

        try {
            for (i = 0; i < conns.length; ++i) {
                conn = conns[i];
                // Bypass the implicit auth call in getDB();
                var db = new DB(conn, dbName);
                try {
                    retryOnNetworkError(db._authOrThrow.bind(db, authParams));
                } catch (ex3) {
                    doassert("assert failed : " + "Failed to authenticate " + conn + " to " +
                             dbName + " using parameters " + tojson(authParams) + " : " + ex3);
                }
            }
        } catch (ex) {
            try {
                authutil.logout(conns, dbName);
            } catch (ex2) {
            }
            throw ex;
        }
    };

    /**
    * Authenticates all connections in "conns" using "authParams" on database "dbName".
    * Raises in exception if any of the authentications succeed.
    */
    authutil.assertAuthenticateFails = function(conns, dbName, authParams) {
        var conn, i;
        if (conns.length == null)
            conns = [conns];

        for (i = 0; i < conns.length; ++i) {
            conn = conns[i];
            // Bypass the implicit auth call in getDB();
            var db = new DB(conn, dbName);
            const ex = assert.throws(retryOnNetworkError,
                                     [db._authOrThrow.bind(db, authParams)],
                                     "Unexpectedly authenticated " + conn + " to " + dbName +
                                         " using parameters " + tojson(authParams));
            if (isNetworkError(ex)) {
                throw ex;
            }
        }
    };

    /**
     * Executes action() after authenticating the keyfile user on "conn", then logs out the keyfile
     * user.
     */
    authutil.asCluster = function(conn, keyfile, action) {
        var ex;
        const authMode = jsTest.options().clusterAuthMode;

        if (authMode === 'keyFile') {
            authutil.assertAuthenticate(conn, 'admin', {
                user: '__system',
                mechanism: 'SCRAM-SHA-1',
                pwd: cat(keyfile).replace(/[\011-\015\040]/g, '')
            });
        } else if (authMode === 'x509') {
            authutil.assertAuthenticate(conn, '$external', {
                mechanism: 'MONGODB-X509',
            });
        } else {
            throw new Error('clusterAuthMode ' + authMode + ' is currently unsupported');
        }

        try {
            return action();
        } finally {
            try {
                authutil.logout(conn, 'admin');
            } catch (ex) {
            }
        }
    };

}());
