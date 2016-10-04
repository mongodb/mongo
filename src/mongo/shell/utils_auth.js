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
            conn[i].getDB(dbname).logout();
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
                assert(conn.getDB(dbName).auth(authParams),
                       "Failed to authenticate " + conn + " to " + dbName + " using parameters " +
                           tojson(authParams));
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
            assert(!conn.getDB(dbName).auth(authParams),
                   "Unexpectedly authenticated " + conn + " to " + dbName + " using parameters " +
                       tojson(authParams));
        }
    };

    /**
     * Executes action() after authenticating the keyfile user on "conn", then logs out the keyfile
     * user.
     */
    authutil.asCluster = function(conn, keyfile, action) {
        var ex;
        authutil.assertAuthenticate(conn, 'admin', {
            user: '__system',
            mechanism: 'SCRAM-SHA-1',
            pwd: cat(keyfile).replace(/[\011-\015\040]/g, '')
        });

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
