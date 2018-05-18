// @tags: [requires_replication, requires_sharding]
(function() {
    'use strict';

    // This test makes assertions about the number of sessions, which are not compatible with
    // implicit sessions.
    TestData.disableImplicitSessions = true;

    load("jstests/libs/retryable_writes_util.js");

    if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
        jsTestLog("Retryable writes are not supported, skipping test");
        return;
    }

    function Repl(lifetime) {
        this.rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {setParameter: {TransactionRecordMinimumLifetimeMinutes: lifetime}},
        });
        this.rst.startSet();
        this.rst.initiate();
    }

    Repl.prototype.stop = function() {
        this.rst.stopSet();
    };

    Repl.prototype.getConn = function() {
        return this.rst.getPrimary();
    };

    Repl.prototype.getTransactionConn = function() {
        return this.rst.getPrimary();
    };

    function Sharding(lifetime) {
        this.st = new ShardingTest({
            shards: 1,
            mongos: 1,
            config: 1,
            other: {
                rs: true,
                rsOptions: {setParameter: {TransactionRecordMinimumLifetimeMinutes: lifetime}},
                rs0: {nodes: 1},
            },
        });

        this.st.s0.getDB("admin").runCommand({enableSharding: "test"});
        this.st.s0.getDB("admin").runCommand({shardCollection: "test.test", key: {_id: 1}});

        // Ensure that the sessions collection exists.
        this.st.c0.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1});
    }

    Sharding.prototype.stop = function() {
        this.st.stop();
    };

    Sharding.prototype.getConn = function() {
        return this.st.s0;
    };

    Sharding.prototype.getTransactionConn = function() {
        return this.st.rs0.getPrimary();
    };

    const nSessions = 1500;

    function Fixture(impl) {
        this.impl = impl;
        this.conn = impl.getConn();
        this.transactionConn = impl.getTransactionConn();

        this.sessions = [];

        for (var i = 0; i < nSessions; i++) {
            // make a session and get it to the collection
            var session = this.conn.startSession({retryWrites: 1});
            session.getDatabase("test").test.count({});
            this.sessions.push(session);
        }

        this.refresh();
        this.assertOutstandingTransactions(0);
        this.assertOutstandingSessions(nSessions);

        for (var i = 0; i < nSessions; i++) {
            // make a session and get it to the collection
            var session = this.sessions[i];
            assert.writeOK(session.getDatabase("test").test.save({a: 1}));
        }

        // Ensure a write flushes a transaction
        this.assertOutstandingTransactions(nSessions);
        this.assertOutstandingSessions(nSessions);

        // Ensure a refresh/reap doesn't remove the transaction
        this.refresh();
        this.reap();
        this.assertOutstandingTransactions(nSessions);
        this.assertOutstandingSessions(nSessions);
    }

    Fixture.prototype.assertOutstandingTransactions = function(count) {
        assert.eq(count, this.transactionConn.getDB("config").transactions.count());
    };

    Fixture.prototype.assertOutstandingSessions = function(count) {
        assert.eq(count, this.getDB("config").system.sessions.count());
    };

    Fixture.prototype.refresh = function() {
        assert.commandWorked(this.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));
    };

    Fixture.prototype.reap = function() {
        assert.commandWorked(
            this.transactionConn.getDB("admin").runCommand({reapLogicalSessionCacheNow: 1}));
    };

    Fixture.prototype.getDB = function(db) {
        return this.conn.getDB(db);
    };

    Fixture.prototype.stop = function() {
        this.sessions.forEach(function(session) {
            session.endSession();
        });
        return this.impl.stop();
    };

    [Repl, Sharding].forEach(function(Impl) {
        {
            var fixture = new Fixture(new Impl(-1));
            // Remove a session
            fixture.getDB("config").system.sessions.remove({});
            fixture.assertOutstandingTransactions(nSessions);
            fixture.assertOutstandingSessions(0);

            // See the transaction get reaped as a result
            fixture.reap();
            fixture.assertOutstandingTransactions(0);
            fixture.assertOutstandingSessions(0);

            fixture.stop();
        }

        {
            var fixture = new Fixture(new Impl(30));
            // Remove a session
            fixture.getDB("config").system.sessions.remove({});
            fixture.assertOutstandingTransactions(nSessions);
            fixture.assertOutstandingSessions(0);

            // See the transaction was not reaped as a result
            fixture.reap();
            fixture.assertOutstandingTransactions(nSessions);
            fixture.assertOutstandingSessions(0);

            fixture.stop();
        }
    });
})();
