/**
 * Tests how applyOps handles index specs with unknown fields.
 *
 * We subject index specs with version 2 or later to stricter validation than version 1 index specs.
 * When given an index spec with an unrecognized field, applyOps will reject v:2 indexes with an
 * InvalidIndexSpecificationOption error while v:1 indexes are accepted as-is.
 *
 * @tags: [
 *     requires_non_retryable_commands,
 *     requires_fastcount,
 *
 *     # applyOps uses the oplog which requires replication support.
 *     requires_replication,
 * ]
 */

(function() {
    'use strict';

    const t = db.apply_ops_invalid_index_spec;
    t.drop();

    const collNs = t.getFullName();
    const cmdNs = db.getName() + '.$cmd';
    const systemIndexesNs = db.getCollection('system.indexes').getFullName();

    assert.commandWorked(db.createCollection(t.getName()));
    assert.writeOK(t.save({_id: 100, a: 100}));

    // Tests that db.collection.createIndex() fails when given an index spec containing an unknown
    // field.
    assert.commandFailedWithCode(t.createIndex({a: 1}, {v: 2, name: 'a_1_base_v2', unknown: 1}),
                                 ErrorCodes.InvalidIndexSpecificationOption);
    assert.commandFailedWithCode(t.createIndex({a: 1}, {v: 1, name: 'a_1_base_v1', unknown: 1}),
                                 ErrorCodes.InvalidIndexSpecificationOption);

    // A createIndexes command for a v:2 index with an unknown field in the index spec should fail.
    assert.commandFailedWithCode(db.adminCommand({
        applyOps: [{
            op: 'c',
            ns: cmdNs,
            o: {
                createIndexes: t.getName(),
                v: 2,
                key: {a: 1},
                name: 'a_1_create_v2',
                unknown: 1,
            },
        }],
    }),
                                 ErrorCodes.InvalidIndexSpecificationOption);

    // A createIndexes command for a v:1 index with an unknown field in the index spec should work.
    const res1 = assert.commandWorked(db.adminCommand({
        applyOps: [{
            op: 'c',
            ns: cmdNs,
            o: {
                createIndexes: t.getName(),
                v: 1,
                key: {a: 1},
                name: 'a_1_create_v1',
                unknown: 1,
            },
        }],
    }));

    // Inserting a v:2 index directly into system.indexes with an unknown field in the index
    // spec should return an error.
    assert.commandFailedWithCode(db.adminCommand({
        applyOps: [{
            op: 'i',
            ns: systemIndexesNs,
            o: {v: 2, key: {a: 1}, name: 'a_1_system_v2', ns: collNs, unknown: 1},
        }],
    }),
                                 ErrorCodes.InvalidIndexSpecificationOption);

    // Inserting a v:1 index directly into system.indexes with an unknown field in the index spec
    // should ignore the unrecognized field and create the index.
    assert.commandWorked(db.adminCommand({
        applyOps: [{
            op: 'i',
            ns: systemIndexesNs,
            o: {v: 1, key: {a: 1}, name: 'a_1_system_v1', ns: collNs, unknown: 1},
        }],
    }));
})();
