(function() {
    "use strict";

    load("jstests/libs/get_index_helpers.js");

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
    assert.commandFailed(
        db.adminCommand({applyOps: [{op: 'c', ns: ''}]}),
        'applyOps should fail on non-"n" operation type with empty "ns" field value');

    // Excessively nested applyOps commands gracefully fail.
    assert.commandFailed(db.adminCommand({
        "applyOps": [{
            "ts": {"$timestamp": {"t": 1, "i": 100}},
            "h": 0,
            "v": 2,
            "op": "c",
            "ns": "test.$cmd",
            "o": {
                "applyOps": [{
                    "ts": {"$timestamp": {"t": 1, "i": 100}},
                    "h": 0,
                    "v": 2,
                    "op": "c",
                    "ns": "test.$cmd",
                    "o": {
                        "applyOps": [{
                            "ts": {"$timestamp": {"t": 1, "i": 100}},
                            "h": 0,
                            "v": 2,
                            "op": "c",
                            "ns": "test.$cmd",
                            "o": {
                                "applyOps": [{
                                    "ts": {"$timestamp": {"t": 1, "i": 100}},
                                    "h": 0,
                                    "v": 2,
                                    "op": "c",
                                    "ns": "test.$cmd",
                                    "o": {
                                        "applyOps": [{
                                            "ts": {"$timestamp": {"t": 1, "i": 100}},
                                            "h": 0,
                                            "v": 2,
                                            "op": "c",
                                            "ns": "test.$cmd",
                                            "o": {
                                                "applyOps": [{
                                                    "ts": {"$timestamp": {"t": 1, "i": 100}},
                                                    "h": 0,
                                                    "v": 2,
                                                    "op": "c",
                                                    "ns": "test.$cmd",
                                                    "o": {
                                                        "applyOps": [{
                                                            "ts":
                                                                {"$timestamp": {"t": 1, "i": 100}},
                                                            "h": 0,
                                                            "v": 2,
                                                            "op": "c",
                                                            "ns": "test.$cmd",
                                                            "o": {
                                                                "applyOps": [{
                                                                    "ts": {
                                                                        "$timestamp":
                                                                            {"t": 1, "i": 100}
                                                                    },
                                                                    "h": 0,
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
                                                                            "h": 0,
                                                                            "v": 2,
                                                                            "op": "c",
                                                                            "ns": "test.$cmd",
                                                                            "o": {
                                                                                "applyOps": [{
                                                                                    "ts": {
                                                                                        "$timestamp":
                                                                                            {
                                                                                              "t":
                                                                                                  1,
                                                                                              "i":
                                                                                                  100
                                                                                            }
                                                                                    },
                                                                                    "h": 0,
                                                                                    "v": 2,
                                                                                    "op": "c",
                                                                                    "ns":
                                                                                        "test.$cmd",
                                                                                    "o": {
                                                                                        "applyOps": [
                                                                                            {
                                                                                              "ts": {
                                                                                                  "$timestamp": {
                                                                                                      "t":
                                                                                                          1,
                                                                                                      "i":
                                                                                                          100
                                                                                                  }
                                                                                              },
                                                                                              "h":
                                                                                                  0,
                                                                                              "v":
                                                                                                  2,
                                                                                              "op":
                                                                                                  "c",
                                                                                              "ns":
                                                                                                  "test.$cmd",
                                                                                              "o": {
                                                                                                  "applyOps":
                                                                                                      [
                                                                                                      ]
                                                                                              }
                                                                                            }
                                                                                        ]
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

    // Missing 'o' field value in an operation of type 'i' on 'system.indexes' collection.
    assert.commandFailedWithCode(
        db.adminCommand({applyOps: [{op: 'i', ns: db.getName() + '.system.indexes'}]}),
        ErrorCodes.NoSuchKey,
        'applyOps should fail on system.indexes insert operation without "o" field');

    // Non-object 'o' field value in an operation of type 'i' on 'system.indexes' collection.
    assert.commandFailedWithCode(
        db.adminCommand({applyOps: [{op: 'i', ns: db.getName() + '.system.indexes', o: 'bar'}]}),
        ErrorCodes.TypeMismatch,
        'applyOps should fail on system.indexes insert operation with non-object "o" field');

    // Missing 'ns' field in index spec.
    assert.commandFailedWithCode(
        db.adminCommand({
            applyOps: [{
                op: 'i',
                ns: db.getName() + '.system.indexes',
                o: {
                    key: {a: 1},
                    name: 'a_1',
                }
            }]
        }),
        ErrorCodes.NoSuchKey,
        'applyOps should fail on system.indexes insert operation with missing index namespace');

    // Non-string 'ns' field in index spec.
    assert.commandFailedWithCode(
        db.adminCommand({
            applyOps: [{
                op: 'i',
                ns: db.getName() + '.system.indexes',
                o: {
                    ns: 12345,
                    key: {a: 1},
                    name: 'a_1',
                }
            }]
        }),
        ErrorCodes.TypeMismatch,
        'applyOps should fail on system.indexes insert operation with non-string index namespace');

    // Invalid 'ns' field in index spec.
    assert.commandFailedWithCode(
        db.adminCommand({
            applyOps: [{
                op: 'i',
                ns: db.getName() + '.system.indexes',
                o: {
                    ns: 'invalid_namespace',
                    key: {a: 1},
                    name: 'a_1',
                }
            }]
        }),
        ErrorCodes.InvalidNamespace,
        'applyOps should fail on system.indexes insert operation with invalid index namespace');

    // Inconsistent database name in index spec namespace.
    assert.commandFailedWithCode(
        db.adminCommand({
            applyOps: [{
                op: 'i',
                ns: db.getName() + '.system.indexes',
                o: {
                    ns: 'baddbprefix' + t.getFullName(),
                    key: {a: 1},
                    name: 'a_1',
                }
            }]
        }),
        ErrorCodes.InvalidNamespace,
        'applyOps should fail on system.indexes insert operation with index namespace containing ' +
            'inconsistent database name');

    // Valid 'ns' field value in unknown operation type 'x'.
    assert.commandFailed(
        db.adminCommand({applyOps: [{op: 'x', ns: t.getFullName()}]}),
        'applyOps should fail on unknown operation type "x" with valid "ns" value');

    assert.eq(0, t.find().count(), "Non-zero amount of documents in collection to start");

    /**
     * Test function for running CRUD operations on non-existent namespaces using various
     * combinations of invalid namespaces (collection/database), allowAtomic and alwaysUpsert.
     *
     * Leave 'expectedErrorCode' undefined if this command is expected to run successfully.
     */
    function testCrudOperationOnNonExistentNamespace(optype, o, o2, expectedErrorCode) {
        expectedErrorCode = expectedErrorCode || ErrorCodes.OK;
        const t2 = db.getSiblingDB('apply_ops1_no_such_db').getCollection('t');
        [t, t2].forEach(coll => {
            const op = {op: optype, ns: coll.getFullName(), o: o, o2: o2};
            [false, true].forEach(allowAtomic => {
                [false, true].forEach(alwaysUpsert => {
                    const cmd = {
                        applyOps: [op],
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
    }

    // Insert and update operations on non-existent collections/databases should return
    // NamespaceNotFound.
    testCrudOperationOnNonExistentNamespace('i', {_id: 0}, {}, ErrorCodes.NamespaceNotFound);
    testCrudOperationOnNonExistentNamespace('u', {x: 0}, {_id: 0}, ErrorCodes.NamespaceNotFound);

    // Delete operations on non-existent collections/databases should return OK for idempotency
    // reasons.
    testCrudOperationOnNonExistentNamespace('d', {_id: 0}, {});

    assert.commandWorked(db.createCollection(t.getName()));
    var a = assert.commandWorked(
        db.adminCommand({applyOps: [{"op": "i", "ns": t.getFullName(), "o": {_id: 5, x: 17}}]}));
    assert.eq(1, t.find().count(), "Valid insert failed");
    assert.eq(true, a.results[0], "Bad result value for valid insert");

    a = assert.commandWorked(
        db.adminCommand({applyOps: [{"op": "i", "ns": t.getFullName(), "o": {_id: 5, x: 17}}]}));
    assert.eq(1, t.find().count(), "Duplicate insert failed");
    assert.eq(true, a.results[0], "Bad result value for duplicate insert");

    var o = {_id: 5, x: 17};
    assert.eq(o, t.findOne(), "Mismatching document inserted.");

    // 'o' field is an empty array.
    assert.commandFailed(db.adminCommand({applyOps: [{op: 'i', ns: t.getFullName(), o: []}]}),
                         'applyOps should fail on insert of object with empty array element');

    var res = assert.commandWorked(db.runCommand({
        applyOps: [
            {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$inc: {x: 1}}},
            {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$inc: {x: 1}}}
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
            {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$inc: {x: 1}}},
            {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$inc: {x: 1}}}
        ],
        preCondition: [{ns: t.getFullName(), q: {_id: 5}, res: {x: 19}}]
    });

    o.x++;
    o.x++;

    assert.eq(1, t.find().count(), "Updates increased number of documents");
    assert.eq(o, t.findOne(), "Document doesn't match expected");
    assert.eq(true, res.results[0], "Bad result value for valid update");
    assert.eq(true, res.results[1], "Bad result value for valid update");

    // preCondition doesn't match ns
    res = db.runCommand({
        applyOps: [
            {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$inc: {x: 1}}},
            {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$inc: {x: 1}}}
        ],
        preCondition: [{ns: "foo.otherName", q: {_id: 5}, res: {x: 21}}]
    });

    assert.eq(o, t.findOne(), "preCondition didn't match, but ops were still applied");

    // preCondition doesn't match query
    res = db.runCommand({
        applyOps: [
            {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$inc: {x: 1}}},
            {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$inc: {x: 1}}}
        ],
        preCondition: [{ns: t.getFullName(), q: {_id: 5}, res: {x: 19}}]
    });

    assert.eq(o, t.findOne(), "preCondition didn't match, but ops were still applied");

    res = db.runCommand({
        applyOps: [
            {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$inc: {x: 1}}},
            {op: "u", ns: t.getFullName(), o2: {_id: 6}, o: {$inc: {x: 1}}}
        ]
    });

    assert.eq(true, res.results[0], "Valid update failed");
    assert.eq(true, res.results[1], "Valid update failed");

    // Foreground index build.
    res = assert.commandWorked(db.adminCommand({
        applyOps: [{
            "op": "i",
            "ns": db.getName() + ".system.indexes",
            "o": {
                ns: t.getFullName(),
                key: {a: 1},
                name: "a_1",
            }
        }]
    }));
    assert.eq(1, res.applied, "Incorrect number of operations applied");
    assert.eq(true, res.results[0], "Foreground index creation failed");
    var allIndexes = t.getIndexes();
    var spec = GetIndexHelpers.findByName(allIndexes, "a_1");
    assert.neq(null, spec, "Foreground index 'a_1' not found: " + tojson(allIndexes));
    assert.eq(1, spec.v, "Expected v=1 index to be built since 'v' field was omitted");

    // Background indexes are created in the foreground when processed by applyOps.
    res = assert.commandWorked(db.adminCommand({
        applyOps: [{
            "op": "i",
            "ns": db.getName() + ".system.indexes",
            "o": {
                ns: t.getFullName(),
                key: {b: 1},
                name: "b_1",
                background: true,
            }
        }]
    }));
    assert.eq(1, res.applied, "Incorrect number of operations applied");
    assert.eq(true, res.results[0], "Background index creation failed");
    allIndexes = t.getIndexes();
    spec = GetIndexHelpers.findByName(allIndexes, "b_1");
    assert.neq(null, spec, "Background index 'b_1' not found: " + tojson(allIndexes));
    assert.eq(1, spec.v, "Expected v=1 index to be built since 'v' field was omitted");

    // Foreground v=2 index build.
    res = assert.commandWorked(db.adminCommand({
        applyOps: [{
            "op": "i",
            "ns": db.getName() + ".system.indexes",
            "o": {
                ns: t.getFullName(),
                key: {c: 1},
                name: "c_1",
                v: 2,
            }
        }]
    }));
    assert.eq(1, res.applied, "Incorrect number of operations applied");
    assert.eq(true, res.results[0], "Foreground v=2 index creation failed");
    allIndexes = t.getIndexes();
    spec = GetIndexHelpers.findByName(allIndexes, "c_1");
    assert.neq(null, spec, "Foreground index 'c_1' not found: " + tojson(allIndexes));
    assert.eq(2, spec.v, "Expected v=2 index to be built");
})();
