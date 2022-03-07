'use strict';

/**
 * Tests killing time-series inserts.
 *
 * @tags: [
 *   requires_timeseries,
 *   # killOp does not support stepdowns.
 *   does_not_support_stepdowns,
 *   # Timeseries do not support multi-document transactions with inserts.
 *   does_not_support_transactions,
 *   # Kill operations do not propagate for writes on mongos.
 *   assumes_unsharded_collection
 * ]
 */

var $config = (function() {
    const timeFieldName = 'time';
    const metaFieldName = 'tag';

    function getCollectionName(collName) {
        return jsTestName() + "_" + collName;
    }

    const insert = function(db, collName, ordered) {
        const docs = [];
        for (let i = 0; i < 1000; ++i) {
            docs.push({[timeFieldName]: ISODate(), [metaFieldName]: i % 10});
        }
        // TODO (SERVER-44673): Use assert.commandWorkedOrFailedWithCode without special handling.
        const res = db.runCommand({insert: collName, documents: docs, ordered: ordered});
        if (res.hasOwnProperty('writeErrors')) {
            for (const writeError of res.writeErrors) {
                assert.eq(writeError.code, ErrorCodes.Interrupted, tojson(res));
            }
        } else if (res.hasOwnProperty('writeConcernError')) {
            assert.eq(res.writeConcernError.code, ErrorCodes.Interrupted, tojson(res));
        } else {
            assert.commandWorkedOrFailedWithCode(res, ErrorCodes.Interrupted, tojson(res));
        }
    };

    const states = {
        init: function(db, collName) {},

        insertOrdered: function(db, collNameSuffix) {
            let collName = getCollectionName(collNameSuffix);
            insert(db, collName, true);
        },

        insertUnordered: function(db, collNameSuffix) {
            let collName = getCollectionName(collNameSuffix);
            insert(db, collName, false);
        },

        killInsert: function(db, collNameSuffix) {
            let collName = getCollectionName(collNameSuffix);
            const inprog =
                assert.commandWorked(db.currentOp({ns: db[collName].getFullName(), op: 'insert'}))
                    .inprog;
            if (inprog.length) {
                assert.commandWorked(
                    db.adminCommand({killOp: 1, op: inprog[Random.randInt(inprog.length)].opid}));
            }
        },
    };

    const setup = function(db, collNameSuffix) {
        let collName = getCollectionName(collNameSuffix);
        assert.commandWorked(db.createCollection(
            collName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    };

    const standardTransition = {
        insertOrdered: 0.4,
        insertUnordered: 0.4,
        killInsert: 0.2,
    };

    const transitions = {
        init: standardTransition,
        insertOrdered: standardTransition,
        insertUnordered: standardTransition,
        killInsert: standardTransition,
    };

    return {
        threadCount: 10,
        iterations: 100,
        setup: setup,
        states: states,
        transitions: transitions,
    };
})();
