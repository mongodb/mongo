var AuthSupport;

(function () {

    assert(!AuthSupport, "Double-load of auth_support.js detected!");
    AuthSupport = {};

    /**
     * Logs out all connections "conn" from database "dbname".
     */
    var logout = function AuthSupport_logout(conn, dbname) {
        var i;
        if (null == conn.length) {
            conn = [ conn ];
        }
        for (i = 0; i < conn.length; ++i) {
            conn[i].getDB(dbname).logout();
        }
    };
    AuthSupport.logout = logout;

    /**
     * Authenticates all connections in "conns" using "authParams" on database "dbName".
     *
     * Raises an exception if any authentication fails, and tries to leave all connnections
     * in "conns" in the logged-out-of-dbName state.
     */
    var assertAuthenticate = function AuthSupport_assertAuthenticate(conns, dbName, authParams) {

        var conn, i, ex, ex2;
        if (conns.length == null)
            conns = [ conns ];

        try {
            for (i = 0; i < conns.length; ++i) {
                conn = conns[i];
                assert(conn.getDB(dbName).auth(authParams),
                       "Failed to authenticate " + conn + " to " + dbName + " using parameters " +
                       tojson(authParams));
            }
        }
        catch (ex) {
            try {
                logout(conns, dbName);
            }
            catch (ex2) {
            }
            throw ex;
        }
    };
    AuthSupport.assertAuthenticate = assertAuthenticate;

    /**
     * Authenticates all connections in "conns" using "authParams" on database "dbName".
     * Raises in exception if any of the authentications succeed.
     */
    var assertAuthenticateFails = function AuthSupport_assertAuthenticateFails(
            conns, dbName, authParams) {

        var conn, i;
        if (conns.length == null)
            conns = [ conns ];

        for (i = 0; i < conns.length; ++i) {
            conn = conns[i];
            assert(!conn.getDB(dbName).auth(authParams),
                   "Unexpectedly authenticated " + conn + " to " + dbName + " using parameters " +
                   tojson(authParams));
        }
    };
    AuthSupport.assertAuthenticateFails = assertAuthenticateFails;

    /**
     * Executes action() after authenticating the keyfile user on "conn", then logs out the keyfile
     * user.
     */
    var asCluster = function AuthSupport_asCluster(conn, keyfile, action) {
        var ex;
        assertAuthenticate(conn, 'local', {
            user: '__system',
            mechanism: 'MONGODB-CR',
            pwd: cat(keyfile).replace(/[ \n]/g, '')
        });

        try {
            action();
        }
        finally {
            try {
                logout(conn, 'local');
            }
            catch (ex) {
            }
        }
    };
    AuthSupport.asCluster = asCluster;

    // Update ReplSetTest.prototype.waitForIndicator to authenticate connections to the
    // replica set members using the keyfile, before attempting to perform operations.
    (function updateReplsetTestPrototypes() {
        var originalWaitForIndicator = ReplSetTest.prototype.waitForIndicator;
        ReplSetTest.prototype.waitForIndicator = function newRSTestWaitForIndicator(
            node, states, ind, timeout) {

            var self = this;
            if (node.length)
                return originalWaitForIndicator.apply(self, [node, states, ind, timeout]);
            asCluster(self.getMaster(), self.keyFile, function () {
                originalWaitForIndicator.apply(self, [node, states, ind, timeout]);
            });
        };
    }());

}());
