'use strict';

/**
 * Tests killing time-series inserts.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */

var $config = (function() {
    const timeFieldName = 'time';
    const metaFieldName = 'tag';

    const insert = function(db, collName, ordered) {
        const docs = [];
        for (let i = 0; i < 1000; ++i) {
            docs.push({[timeFieldName]: ISODate(), [metaFieldName]: i % 10});
        }
        // TODO (SERVER-44673): Use assert.commandWorkedOrFailedWithCode without special handling.
        const res = db.runCommand({insert: collName, documents: docs, ordered: ordered});
        if (res.hasOwnProperty('writeErrors') && res.writeErrors.length > 0) {
            assert.eq(res.writeErrors[0].code, ErrorCodes.Interrupted);
        } else if (res.hasOwnProperty('writeConcernError')) {
            assert.eq(res.writeConcernError.code, ErrorCodes.Interrupted);
        } else {
            assert.commandWorkedOrFailedWithCode(res, ErrorCodes.Interrupted);
        }
    };

    const states = {
        init: function(db, collName) {},

        insertOrdered: function(db, collName) {
            insert(db, collName, true);
        },

        insertUnordered: function(db, collName) {
            insert(db, collName, false);
        },

        killInsert: function(db, collName) {
            const inprog =
                assert.commandWorked(db.currentOp({ns: db[collName].getFullName(), op: 'insert'}))
                    .inprog;
            if (inprog.length) {
                assert.commandWorked(
                    db.adminCommand({killOp: 1, op: inprog[Random.randInt(inprog.length)].opid}));
            }
        },
    };

    const setup = function(db, collName) {
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
