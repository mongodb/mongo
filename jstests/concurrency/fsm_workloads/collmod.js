'use strict';

/**
 * collmod.js
 *
 * Base workload for collMod command.
 * Generates some random data and inserts it into a collection with a
 * TTL index. Runs a collMod command to change the value of the
 * expireAfterSeconds setting to a random integer.
 *
 * All threads update the same TTL index on the same collection.
 */
var $config = (function() {

    var data = {
        numDocs: 1000,
        maxTTL: 5000, // max time to live
        ttlIndexExists: true
    };

    var states = (function() {

        function collMod(db, collName) {
            var newTTL = Random.randInt(this.maxTTL);
            var res = db.runCommand({ collMod: this.threadCollName,
                                      index: {
                                          keyPattern: { createdAt: 1 },
                                          expireAfterSeconds: newTTL
                                      }
                                    });
            assertAlways.commandWorked(res);
            // only assert if new expireAfterSeconds differs from old one
            if (res.hasOwnProperty('expireAfterSeconds_new')) {
                assertWhenOwnDB.eq(res.expireAfterSeconds_new, newTTL);
            }
        }

        return {
            collMod: collMod
        };

    })();

    var transitions = {
        collMod: { collMod: 1 }
    };

    function setup(db, collName, cluster) {
        // other workloads that extend this one might have set 'this.threadCollName'
        this.threadCollName = this.threadCollName || collName;
        var bulk = db[this.threadCollName].initializeUnorderedBulkOp();
        for (var i = 0; i < this.numDocs; ++i) {
            bulk.insert({ createdAt: new Date() });
        }

        var res = bulk.execute();
        assertAlways.writeOK(res);
        assertAlways.eq(this.numDocs, res.nInserted);

        // create TTL index
        res = db[this.threadCollName].ensureIndex({ createdAt: 1 },
                                                  { expireAfterSeconds: 3600 });
        assertAlways.commandWorked(res);
    }

    return {
        threadCount: 10,
        iterations: 20,
        data: data,
        startState: 'collMod',
        states: states,
        transitions: transitions,
        setup: setup
    };

})();
