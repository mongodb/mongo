/**
 * Assert that arbiters periodically clear out their logical session cache. Inability to do so
 * prohibits new clients from connecting.
 *
 * @tags: [requires_replication]
 */
(function() {
    const name = "server54064";
    const replSet = new ReplSetTest({
        name: name,
        nodes: 2,
        // `disableLogicalSessionCacheRefresh` is true by default, but is disabled for testing
        // purposes
        // in servers spawned from the shell.
        nodeOptions: {
            setParameter: {
                // Each refresh on an arbiter will empty the cache.
                logicalSessionRefreshMillis: 10000,
                // The number of connections/sessions before the cache is full, prohibiting new
                // clients
                // from connecting.
                maxSessions: 3,
                disableLogicalSessionCacheRefresh: false
            }
        }
    });
    const nodes = replSet.nodeList();

    replSet.startSet();
    replSet.initiate({
        _id: name,
        members: [{_id: 0, host: nodes[0]}, {_id: 1, host: nodes[1], arbiterOnly: true}],
    });

    let arbConn = replSet.getArbiter();
    assert.soon(() => {
        // Connect to mongo in a tight loop to exhaust the number of available logical sessions in
        // the
        // cache.
        const result = new Mongo(arbConn.host).adminCommand({hello: 1});
        if (result['ok'] === 0) {
            assert.commandFailedWithCode(result, ErrorCodes.TooManyLogicalSessions);
            return true;
        }
        return false;
    });

    assert.soon(() => {
        // Once we observe that the number of sessions is maxed out, loop again to confirm that the
        // cache eventually does get cleared.
        const result = new Mongo(arbConn.host).adminCommand({hello: 1});
        if (result['ok'] === 0) {
            assert.commandFailedWithCode(result, ErrorCodes.TooManyLogicalSessions);
            return false;
        }
        return true;
    });

    replSet.stopSet();
})();
