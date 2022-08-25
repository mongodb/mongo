/*
 * @tags: [
 *   assumes_superuser_permissions,
 *   requires_fastcount,
 *   requires_non_retryable_commands,
 *   # applyOps is not supported on mongos
 *   assumes_against_mongod_not_mongos,
 *   # applyOps uses the oplog that require replication support
 *   requires_replication,
 *   # Tenant migrations don't support applyOps.
 *   tenant_migration_incompatible,
 *   # This test will fail on a mixed 6.1/6.0 cluster because a 6.0 node can successfully apply
 *   # a $v: 1 oplog entry, but a 6.1 node cannot successfully replicate it. This isn't an issue
 *   # because applyOps is an internal command, so users aren't expected to manually insert $v: 1
 *   # entries into the oplog.
 *   requires_fcv_61,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/index_catalog_helpers.js");

var t = db.apply_ops1;
t.drop();

//
// Input validation tests
//

// Empty array of operations.
assert.commandWorked(db.adminCommand({applyOps: []}),
                     'applyOps should not fail on empty array of operations');

// Non-array type for operations.
assert.commandFailed(db.adminCommand({applyOps: "not an array"}),
                     'applyOps should fail on non-array type for operations');

// Missing 'op' field in an operation.
assert.commandFailed(db.adminCommand({applyOps: [{ns: t.getFullName()}]}),
                     'applyOps should fail on operation without "op" field');

// Non-string 'op' field in an operation.
assert.commandFailed(db.adminCommand({applyOps: [{op: 12345, ns: t.getFullName()}]}),
                     'applyOps should fail on operation with non-string "op" field');

// Empty 'op' field value in an operation.
assert.commandFailed(db.adminCommand({applyOps: [{op: '', ns: t.getFullName()}]}),
                     'applyOps should fail on operation with empty "op" field value');

// Missing 'ns' field in an operation.
assert.commandFailed(db.adminCommand({applyOps: [{op: 'c'}]}),
                     'applyOps should fail on operation without "ns" field');

// Non-string 'ns' field in an operation.
assert.commandFailed(db.adminCommand({applyOps: [{op: 'c', ns: 12345}]}),
                     'applyOps should fail on operation with non-string "ns" field');

// Empty 'ns' field value in an operation of type 'n' (noop).
assert.commandWorked(db.adminCommand({applyOps: [{op: 'n', ns: ''}]}),
                     'applyOps should work on no op operation with empty "ns" field value');

// Missing dbname in 'ns' field.
assert.commandFailed(db.adminCommand({applyOps: [{op: 'd', ns: t.getName(), o: {_id: 1}}]}));

// Missing 'o' field value in an operation of type 'c' (command).
assert.commandFailed(db.adminCommand({applyOps: [{op: 'c', ns: t.getFullName()}]}),
                     'applyOps should fail on command operation without "o" field');

// Non-object 'o' field value in an operation of type 'c' (command).
assert.commandFailed(db.adminCommand({applyOps: [{op: 'c', ns: t.getFullName(), o: 'bar'}]}),
                     'applyOps should fail on command operation with non-object "o" field');

// Empty object 'o' field value in an operation of type 'c' (command).
assert.commandFailed(db.adminCommand({applyOps: [{op: 'c', ns: t.getFullName(), o: {}}]}),
                     'applyOps should fail on command operation with empty object "o" field');

// Unknown key in 'o' field value in an operation of type 'c' (command).
assert.commandFailed(db.adminCommand({applyOps: [{op: 'c', ns: t.getFullName(), o: {a: 1}}]}),
                     'applyOps should fail on command operation on unknown key in "o" field');

// Empty 'ns' field value in operation type other than 'n'.
assert.commandFailed(db.adminCommand({applyOps: [{op: 'c', ns: ''}]}),
                     'applyOps should fail on non-"n" operation type with empty "ns" field value');

// Excessively nested applyOps commands gracefully fail.
assert.commandFailed(db.adminCommand({
    "applyOps": [{
        "ts": {"$timestamp": {"t": 1, "i": 100}},
        "v": 2,
        "op": "c",
        "ns": "test.$cmd",
        "o": {
            "applyOps": [{
                "ts": {"$timestamp": {"t": 1, "i": 100}},
                "v": 2,
                "op": "c",
                "ns": "test.$cmd",
                "o": {
                    "applyOps": [{
                        "ts": {"$timestamp": {"t": 1, "i": 100}},
                        "v": 2,
                        "op": "c",
                        "ns": "test.$cmd",
                        "o": {
                            "applyOps": [{
                                "ts": {"$timestamp": {"t": 1, "i": 100}},
                                "v": 2,
                                "op": "c",
                                "ns": "test.$cmd",
                                "o": {
                                    "applyOps": [{
                                        "ts": {"$timestamp": {"t": 1, "i": 100}},
                                        "v": 2,
                                        "op": "c",
                                        "ns": "test.$cmd",
                                        "o": {
                                            "applyOps": [{
                                                "ts": {"$timestamp": {"t": 1, "i": 100}},
                                                "v": 2,
                                                "op": "c",
                                                "ns": "test.$cmd",
                                                "o": {
                                                    "applyOps": [{
                                                        "ts": {"$timestamp": {"t": 1, "i": 100}},
                                                        "v": 2,
                                                        "op": "c",
                                                        "ns": "test.$cmd",
                                                        "o": {
                                                            "applyOps": [{
                                                                "ts": {
                                                                    "$timestamp": {"t": 1, "i": 100}
                                                                },
                                                                "v": 2,
                                                                "op": "c",
                                                                "ns": "test.$cmd",
                                                                "o": {
                                                                    "applyOps": [{
                                                                        "ts": {
                                                                            "$timestamp":
                                                                                {"t": 1, "i": 100}
                                                                        },
                                                                        "v": 2,
                                                                        "op": "c",
                                                                        "ns": "test.$cmd",
                                                                        "o": {
                                                                            "applyOps": [{
                                                                                "ts": {
                                                                                    "$timestamp": {
                                                                                        "t": 1,
                                                                                        "i": 100
                                                                                    }
                                                                                },
                                                                                "v": 2,
                                                                                "op": "c",
                                                                                "ns": "test.$cmd",
                                                                                "o": {
                                                                                    "applyOps": [{
                                                                                        "ts": {
                                                                                            "$timestamp": {
                                                                                                "t":
                                                                                                    1,
                                                                                                "i":
                                                                                                    100
                                                                                            }
                                                                                        },
                                                                                        "v": 2,
                                                                                        "op": "c",
                                                                                        "ns":
                                                                                            "test.$cmd",
                                                                                        "o": {
                                                                                            "applyOps":
                                                                                                []
                                                                                        }
                                                                                    }]
                                                                                }
                                                                            }]
                                                                        }
                                                                    }]
                                                                }
                                                            }]
                                                        }
                                                    }]
                                                }
                                            }]
                                        }
                                    }]
                                }
                            }]
                        }
                    }]
                }
            }]
        }
    }]
}),
                     "Excessively nested applyOps should be rejected");

// Valid 'ns' field value in unknown operation type 'x'.
assert.commandFailed(db.adminCommand({applyOps: [{op: 'x', ns: t.getFullName()}]}),
                     'applyOps should fail on unknown operation type "x" with valid "ns" value');

assert.eq(0, t.find().count(), "Non-zero amount of documents in collection to start");

/**
 * Test function for running CRUD operations on non-existent namespaces using various
 * combinations of invalid namespaces (collection/database), allowAtomic and alwaysUpsert,
 * and nesting.
 *
 * Leave 'expectedErrorCode' undefined if this command is expected to run successfully.
 */
function testCrudOperationOnNonExistentNamespace(optype, o, o2, expectedErrorCode) {
    expectedErrorCode = expectedErrorCode || ErrorCodes.OK;
    const t2 = db.getSiblingDB('apply_ops1_no_such_db').getCollection('t');
    [t, t2].forEach(coll => {
        const op = {op: optype, ns: coll.getFullName(), o: o, o2: o2};
        [false, true].forEach(nested => {
            const opToRun = nested ? {op: 'c', ns: 'test.$cmd', o: {applyOps: [op]}, o2: {}} : op;
            [false, true].forEach(allowAtomic => {
                [false, true].forEach(alwaysUpsert => {
                    const cmd = {
                        applyOps: [opToRun],
                        allowAtomic: allowAtomic,
                        alwaysUpsert: alwaysUpsert
                    };
                    jsTestLog('Testing applyOps on non-existent namespace: ' + tojson(cmd));
                    if (expectedErrorCode === ErrorCodes.OK) {
                        assert.commandWorked(db.adminCommand(cmd));
                    } else {
                        assert.commandFailedWithCode(db.adminCommand(cmd), expectedErrorCode);
                    }
                });
            });
        });
    });
}

// Insert and update operations on non-existent collections/databases should return
// NamespaceNotFound.
testCrudOperationOnNonExistentNamespace('i', {_id: 0}, {}, ErrorCodes.NamespaceNotFound);
testCrudOperationOnNonExistentNamespace(
    'u', {$v: 2, diff: {x: 0}}, {_id: 0}, ErrorCodes.NamespaceNotFound);

// TODO(SERVER-46221): These oplog entries are inserted as given.  After SERVER-21700 and with
// steady-state oplog constraint enforcement on, they will result in secondary crashes.  We
// will need to have applyOps apply operations as executed rather than as given for this to
// work properly.
if (false) {
    // Delete operations on non-existent collections/databases should return OK for idempotency
    // reasons.
    testCrudOperationOnNonExistentNamespace('d', {_id: 0}, {});
}

assert.commandWorked(db.createCollection(t.getName()));
var a = assert.commandWorked(
    db.adminCommand({applyOps: [{"op": "i", "ns": t.getFullName(), "o": {_id: 5, x: 17}}]}));
assert.eq(1, t.find().count(), "Valid insert failed");
assert.eq(true, a.results[0], "Bad result value for valid insert");

// TODO(SERVER-46221): Duplicate inserts result in invalid oplog entries, as above.
if (false) {
    a = assert.commandWorked(
        db.adminCommand({applyOps: [{"op": "i", "ns": t.getFullName(), "o": {_id: 5, x: 17}}]}));
    assert.eq(1, t.find().count(), "Duplicate insert failed");
    assert.eq(true, a.results[0], "Bad result value for duplicate insert");
}

var o = {_id: 5, x: 17};
assert.eq(o, t.findOne(), "Mismatching document inserted.");

// 'o' field is an empty array.
assert.commandFailed(db.adminCommand({applyOps: [{op: 'i', ns: t.getFullName(), o: []}]}),
                     'applyOps should fail on insert of object with empty array element');

var res = assert.commandWorked(db.runCommand({
    applyOps: [
        {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$v: 2, diff: {u: {x: 18}}}},
        {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$v: 2, diff: {u: {x: 19}}}}
    ]
}));

o.x++;
o.x++;

assert.eq(1, t.find().count(), "Updates increased number of documents");
assert.eq(o, t.findOne(), "Document doesn't match expected");
assert.eq(true, res.results[0], "Bad result value for valid update");
assert.eq(true, res.results[1], "Bad result value for valid update");

// preCondition fully matches
res = db.runCommand({
    applyOps: [
        {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$v: 2, diff: {u: {x: 20}}}},
        {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$v: 2, diff: {u: {x: 21}}}}
    ],
    preCondition: [{ns: t.getFullName(), q: {_id: 5}, res: {x: 19}}]
});

// The use of preCondition requires applyOps to run atomically. Therefore, it is incompatible
// with {allowAtomic: false}.
assert.commandFailedWithCode(
    db.runCommand({
        applyOps: [{op: 'u', ns: t.getFullName(), o2: {_id: 5}, o: {$v: 2, diff: {u: {x: 22}}}}],
        preCondition: [{ns: t.getFullName(), q: {_id: 5}, res: {x: 21}}],
        allowAtomic: false,
    }),
    ErrorCodes.InvalidOptions,
    'applyOps should fail when preCondition is present and atomicAllowed is false.');

// The use of preCondition is also incompatible with operations that include commands.
assert.commandFailedWithCode(
    db.runCommand({
        applyOps: [{op: 'c', ns: t.getCollection('$cmd').getFullName(), o: {applyOps: []}}],
        preCondition: [{ns: t.getFullName(), q: {_id: 5}, res: {x: 21}}],
    }),
    ErrorCodes.InvalidOptions,
    'applyOps should fail when preCondition is present and operations includes commands.');

o.x++;
o.x++;

assert.eq(1, t.find().count(), "Updates increased number of documents");
assert.eq(o, t.findOne(), "Document doesn't match expected");
assert.eq(true, res.results[0], "Bad result value for valid update");
assert.eq(true, res.results[1], "Bad result value for valid update");

// preCondition doesn't match ns
res = db.runCommand({
    applyOps: [
        {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$v: 2, diff: {u: {x: 22}}}},
        {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$v: 2, diff: {u: {x: 23}}}}
    ],
    preCondition: [{ns: "foo.otherName", q: {_id: 5}, res: {x: 21}}]
});

assert.eq(o, t.findOne(), "preCondition didn't match, but ops were still applied");

// preCondition doesn't match query
res = db.runCommand({
    applyOps: [
        {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$v: 2, diff: {u: {x: 22}}}},
        {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$v: 2, diff: {u: {x: 23}}}}
    ],
    preCondition: [{ns: t.getFullName(), q: {_id: 5}, res: {x: 19}}]
});

assert.eq(o, t.findOne(), "preCondition didn't match, but ops were still applied");

res = db.runCommand({
    applyOps: [
        {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$v: 2, diff: {u: {x: 22}}}},
        {op: "u", ns: t.getFullName(), o2: {_id: 6}, o: {$v: 2, diff: {u: {x: 23}}}}
    ]
});

assert.eq(true, res.results[0], "Valid update failed");
assert.eq(true, res.results[1], "Valid update failed");

// Ops with transaction numbers are valid.
const lsid = {
    "id": UUID("3eea4a58-6018-40b6-8743-6a55783bf902"),
    "uid": BinData(0, "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=")
};
res = db.runCommand({
    applyOps: [
        {
            op: "i",
            ns: t.getFullName(),
            o: {_id: 7, x: 24},
            lsid: lsid,
            txnNumber: NumberLong(1),
            stmtId: NumberInt(0)
        },
        {
            op: "u",
            ns: t.getFullName(),
            o2: {_id: 8},
            o: {$v: 2, diff: {u: {x: 25}}},
            lsid: lsid,
            txnNumber: NumberLong(1),
            stmtId: NumberInt(1)
        },
        {
            op: "d",
            ns: t.getFullName(),
            o: {_id: 7},
            lsid: lsid,
            txnNumber: NumberLong(2),
            stmtId: NumberInt(0)
        },
    ]
});

assert.eq(true, res.results[0], "Valid insert with transaction number failed");
assert.eq(true, res.results[1], "Valid update with transaction number failed");
assert.eq(true, res.results[2], "Valid delete with transaction number failed");

// Ops with multiple statement IDs are valid.
res = db.runCommand({
    applyOps: [
        {
            op: "i",
            ns: t.getFullName(),
            o: {_id: 7, x: 24},
            lsid: lsid,
            txnNumber: NumberLong(3),
            stmtId: [NumberInt(0), NumberInt(1)]
        },
        {
            op: "u",
            ns: t.getFullName(),
            o2: {_id: 8},
            o: {$v: 2, diff: {u: {x: 25}}},
            lsid: lsid,
            txnNumber: NumberLong(3),
            stmtId: [NumberInt(2), NumberInt(3)]
        },
        {
            op: "d",
            ns: t.getFullName(),
            o: {_id: 7},
            lsid: lsid,
            txnNumber: NumberLong(4),
            stmtId: [NumberInt(0), NumberInt(1)]
        },
    ]
});

assert.eq(true, res.results[0], "Valid insert with multiple statement IDs failed");
assert.eq(true, res.results[1], "Valid update with multiple statement IDs failed");
assert.eq(true, res.results[2], "Valid delete with multiple statement IDs failed");

// When applying a "u" (update) op in the $v: 2 format, the
res = assert.commandWorked(db.adminCommand({
    applyOps: [
        {"op": "i", "ns": t.getFullName(), "o": {_id: 9}},
        {"op": "u", "ns": t.getFullName(), "o2": {_id: 9}, "o": {$v: 2, diff: {u: {z: 1, a: 2}}}},
    ]
}));
assert.eq(t.findOne({_id: 9}), {_id: 9, z: 1, a: 2});  // Note: 'a' and 'z' have been sorted.

// 'ModifierInterface' semantics are not supported, so an update with {$v: 0} should fail.
res = assert.commandFailed(db.adminCommand({
    applyOps: [
        {"op": "i", "ns": t.getFullName(), "o": {_id: 7}},
        {
            "op": "u",
            "ns": t.getFullName(),
            "o2": {_id: 7},
            "o": {$v: NumberInt(0), $set: {z: 1, a: 2}}
        }
    ]
}));
assert.eq(res.code, 4772600);

// When we explicitly specify {$v: 1} it should fail because this version is no longer supported.
assert.commandFailedWithCode(db.adminCommand({
    applyOps: [
        {"op": "i", "ns": t.getFullName(), "o": {_id: 10}},
        {
            "op": "u",
            "ns": t.getFullName(),
            "o2": {_id: 10},
            "o": {$v: NumberInt(1), $set: {z: 1, a: 2}}
        }
    ]
}),
                             4772600);

// {$v: 2} entries encode diffs differently, and operations are applied in the order specified
// rather than in lexicographic order.
res = assert.commandWorked(db.adminCommand({
    applyOps: [
        {"op": "i", "ns": t.getFullName(), "o": {_id: 11, deleteField: 1}},
        {
            "op": "u",
            "ns": t.getFullName(),
            "o2": {_id: 11},
            // The diff indicates that 'deleteField' will be removed and 'newField' will be added
            // with value "foo".
            "o": {$v: NumberInt(2), diff: {d: {deleteField: false}, i: {newField: "foo"}}}
        }
    ]
}));
assert.eq(t.findOne({_id: 11}), {_id: 11, newField: "foo"});

// {$v: 3} does not exist yet, and we check that trying to use it throws an error.
res = assert.commandFailed(db.adminCommand({
    applyOps: [
        {"op": "i", "ns": t.getFullName(), "o": {_id: 12}},
        {
            "op": "u",
            "ns": t.getFullName(),
            "o2": {_id: 12},
            "o": {$v: NumberInt(3), diff: {d: {deleteField: false}}}
        }
    ]
}));
assert.eq(res.code, 4772600);

var insert_op1 = {_id: 13, x: 'inserted apply ops1'};
var insert_op2 = {_id: 14, x: 'inserted apply ops2'};
assert.commandWorked(db.adminCommand({
    "applyOps": [{
        op: 'c',
        ns: 'admin.$cmd',
        o: {
            "applyOps": [{
                op: 'c',
                ns: 'test.$cmd',
                o: {
                    "applyOps": [
                        {op: 'i', ns: t.getFullName(), o: insert_op1},
                        {op: 'i', ns: t.getFullName(), o: insert_op2}
                    ]
                }
            }],
        }
    }]
}),
                     "Nested apply ops was NOT successful");
assert.eq(t.findOne({_id: 13}), insert_op1);
assert.eq(t.findOne({_id: 14}), insert_op2);

assert.commandWorked(db.adminCommand({
    "applyOps": [{
        op: 'c',
        ns: 'admin.$cmd',
        o: {
            "applyOps": [{
                op: 'c',
                ns: 'test.$cmd',
                o: {
                    "applyOps": [
                        {
                            op: 'u',
                            ns: t.getFullName(),
                            o2: {_id: 13},
                            o: {$v: 2, diff: {u: {x: 'nested apply op update1'}}},
                        },
                        {
                            op: 'u',
                            ns: t.getFullName(),
                            o2: {_id: 14},
                            o: {$v: 2, diff: {u: {x: 'nested apply op update2'}}},
                        }
                    ]
                }
            }],
        }
    }]
}),
                     "Nested apply ops was NOT successful");
assert.eq(t.findOne({_id: 13}), {_id: 13, x: 'nested apply op update1'});
assert.eq(t.findOne({_id: 14}), {_id: 14, x: 'nested apply op update2'});
})();
