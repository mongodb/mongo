'use strict';

/**
 * indexed_insert_ttl.js
 *
 * Creates a TTL index with a short expireAfterSeconds value (5 seconds). Each
 * thread does an insert on each iteration. The first insert done by each
 * thread is marked with an extra field. At the end, we assert that the first
 * doc inserted by each thread is no longer in the collection.
 */
var $config = (function() {

    var states = {
        init: function init(db, collName) {
            var res = db[collName].insert({ indexed_insert_ttl: new ISODate(), first: true });
            assertAlways.writeOK(res);
            assertWhenOwnColl.eq(1, res.nInserted, tojson(res));
        },

        insert: function insert(db, collName) {
            var res = db[collName].insert({ indexed_insert_ttl: new ISODate() });
            assertAlways.writeOK(res);
            assertWhenOwnColl.eq(1, res.nInserted, tojson(res));
        }
    };

    var transitions = {
        init: { insert: 1 },
        insert: { insert: 1 }
    };

    function setup(db, collName, cluster) {
        var res = db[collName].ensureIndex(
            { indexed_insert_ttl: 1 },
            { expireAfterSeconds: this.ttlSeconds });
        assertAlways.commandWorked(res);
    }

    function teardown(db, collName, cluster) {
        // The TTL thread runs every 60 seconds, so for reliability, wait more than ttlSeconds
        // plus a minute.
        sleep((2 * this.ttlSeconds + 70) * 1000);
        assertWhenOwnColl.eq(0, db[collName].find({ first: true }).itcount());
    }

    return {
        threadCount: 20,
        iterations: 200,
        states: states,
        transitions: transitions,
        setup: setup,
        data: {
            ttlSeconds: 5
        },
        teardown: teardown
    };
})();
