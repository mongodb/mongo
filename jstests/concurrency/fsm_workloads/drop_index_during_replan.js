'use strict';

/**
 * drop_index_during_replan.js
 *
 * Sets up a situation where there is a plan cache entry for an index scan plan using {a: 1}. Then,
 * provokes a replan by running a query of the same shape which should use index {b: 1}. At the same
 * time, other threads may be dropping {b: 1}. This tests that the replanning process is robust to
 * index drops.
 */
var $config = (function() {
    let data = {
        collName: 'drop_index_during_replan',
        indexSpecs: [
            {a: 1},
            {b: 1},
        ],
    };

    let states = {
        query: function query(db, collName) {
            const coll = db[collName];
            try {
                // By running this query multiple times, we expect to create an active plan cache
                // entry whose plan uses index {a: 1}.
                for (let i = 0; i < 2; ++i) {
                    assertAlways.eq(
                        coll.find({a: "common_value_a", b: "unique_value_15"}).itcount(), 1);
                }

                // Run a query with the same shape, but with different parameters. For this query,
                // we expect the {a: 1} plan to be evicted during replanning, in favor of {b: 1}.
                // The query may fail due to a concurrent index drop.
                assertAlways.eq(coll.find({a: "unique_value_15", b: "common_value_b"}).itcount(),
                                1);
            } catch (e) {
                // We expect any errors to be due to the query getting killed.
                assertAlways.eq(e.code, ErrorCodes.QueryPlanKilled);
            }
        },

        dropIndex: function dropIndex(db, collName) {
            // Drop the index which we expect to be selected after replanning completes We don't
            // assert that the command succeeded when dropping an index because it's possible
            // another thread has already dropped this index.
            db[collName].dropIndex({b: 1});

            // Recreate the index that was dropped.
            assertAlways.commandWorked(db[collName].createIndex({b: 1}));
        }
    };

    let transitions = {query: {query: 0.8, dropIndex: 0.2}, dropIndex: {query: 1}};

    function setup(db, collName, cluster) {
        this.indexSpecs.forEach(indexSpec => {
            assertAlways.commandWorked(db[collName].createIndex(indexSpec));
        });

        for (let i = 0; i < 200; ++i) {
            assertAlways.commandWorked(
                db[collName].insert({a: "common_value_a", b: "unique_value_" + i}));
            assertAlways.commandWorked(
                db[collName].insert({a: "unique_value_" + i, b: "common_value_b"}));
        }
    }

    return {
        threadCount: 10,
        iterations: 50,
        data: data,
        states: states,
        startState: 'query',
        transitions: transitions,
        setup: setup
    };
})();
