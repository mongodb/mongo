// @tags: [uses_transactions]

(function() {
    "use strict";

    const t = db.do_txn1;

    var session = db.getMongo().startSession();
    db = session.getDatabase("test");
    var txnNumber = 0;

    // Use majority write concern to clear the drop-pending that can cause lock conflicts with
    // transactions.
    t.drop({writeConcern: {w: "majority"}});

    //
    // Input validation tests
    //

    // Empty array of operations.
    assert.commandFailedWithCode(db.adminCommand({doTxn: [], txnNumber: NumberLong(txnNumber++)}),
                                 ErrorCodes.InvalidOptions,
                                 'doTxn should fail on empty array of operations');

    // Non-array type for operations.
    assert.commandFailedWithCode(
        db.adminCommand({doTxn: "not an array", txnNumber: NumberLong(txnNumber++)}),
        ErrorCodes.TypeMismatch,
        'doTxn should fail on non-array type for operations');

    // Missing 'op' field in an operation.
    assert.commandFailedWithCode(
        db.adminCommand(
            {doTxn: [{ns: t.getFullName(), o: {_id: 0}}], txnNumber: NumberLong(txnNumber++)}),
        ErrorCodes.FailedToParse,
        'doTxn should fail on operation without "op" field');

    // Non-string 'op' field in an operation.
    assert.commandFailedWithCode(db.adminCommand({
        doTxn: [{op: 12345, ns: t.getFullName(), o: {_id: 0}}],
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.FailedToParse,
                                 'doTxn should fail on operation with non-string "op" field');

    // Empty 'op' field value in an operation.
    assert.commandFailedWithCode(db.adminCommand({
        doTxn: [{op: '', ns: t.getFullName(), o: {_id: 0}}],
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.FailedToParse,
                                 'doTxn should fail on operation with empty "op" field value');

    // Missing 'ns' field in an operation.
    assert.commandFailedWithCode(
        db.adminCommand({doTxn: [{op: 'u', o: {_id: 0}}], txnNumber: NumberLong(txnNumber++)}),
        ErrorCodes.FailedToParse,
        'doTxn should fail on operation without "ns" field');

    // Missing 'o' field in an operation.
    assert.commandFailedWithCode(
        db.adminCommand(
            {doTxn: [{op: 'u', ns: t.getFullName()}], txnNumber: NumberLong(txnNumber++)}),
        ErrorCodes.FailedToParse,
        'doTxn should fail on operation without "o" field');

    // Non-string 'ns' field in an operation.
    assert.commandFailedWithCode(
        db.adminCommand(
            {doTxn: [{op: 'u', ns: 12345, o: {_id: 0}}], txnNumber: NumberLong(txnNumber++)}),
        ErrorCodes.FailedToParse,
        'doTxn should fail on operation with non-string "ns" field');

    // Missing dbname in 'ns' field.
    assert.commandFailedWithCode(
        db.adminCommand(
            {doTxn: [{op: 'd', ns: t.getName(), o: {_id: 1}}], txnNumber: NumberLong(txnNumber++)}),
        ErrorCodes.InvalidNamespace,
        'doTxn should fail with a missing dbname in the "ns" field value');

    // Empty 'ns' field value.
    assert.commandFailed(
        db.adminCommand(
            {doTxn: [{op: 'u', ns: '', o: {_id: 0}}], txnNumber: NumberLong(txnNumber++)}),
        'doTxn should fail with empty "ns" field value');

    // Valid 'ns' field value in unknown operation type 'x'.
    assert.commandFailedWithCode(
        db.adminCommand({doTxn: [{op: 'x', ns: t.getFullName(), o: {_id: 0}}]}),
        ErrorCodes.FailedToParse,
        'doTxn should fail on unknown operation type "x" with valid "ns" value');

    // Illegal operation type 'n' (no-op).
    assert.commandFailedWithCode(db.adminCommand({
        doTxn: [{op: 'n', ns: t.getFullName(), o: {_id: 0}}],
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.InvalidOptions,
                                 'doTxn should fail on "no op" operations.');

    // Illegal operation type 'c' (command).
    assert.commandFailedWithCode(db.adminCommand({
        doTxn: [{op: 'c', ns: t.getCollection('$cmd').getFullName(), o: {applyOps: []}}],
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.InvalidOptions,
                                 'doTxn should fail on commands.');

    // No transaction number in an otherwise valid operation.
    assert.commandFailedWithCode(
        db.adminCommand({doTxn: [{"op": "i", "ns": t.getFullName(), "o": {_id: 5, x: 17}}]}),
        ErrorCodes.InvalidOptions,
        'doTxn should fail when no transaction number is given.');

    // Session IDs and transaction numbers on sub-ops are not allowed
    var lsid = {id: UUID()};
    res = assert.commandFailedWithCode(
        db.runCommand({
            doTxn: [{
                op: "i",
                ns: t.getFullName(),
                o: {_id: 7, x: 24},
                lsid: lsid,
                txnNumber: NumberLong(1),
            }],
            txnNumber: NumberLong(txnNumber++)
        }),
        ErrorCodes.FailedToParse,
        'doTxn should fail when inner transaction contains session id.');

    res = assert.commandFailedWithCode(
        db.runCommand({
            doTxn: [{
                op: "u",
                ns: t.getFullName(),
                o2: {_id: 7},
                o: {$set: {x: 25}},
                txnNumber: NumberLong(1),
            }],
            txnNumber: NumberLong(txnNumber++)
        }),
        ErrorCodes.FailedToParse,
        'doTxn should fail when inner transaction contains transaction number.');

    res = assert.commandFailedWithCode(
        db.runCommand({
            doTxn: [{
                op: "d",
                ns: t.getFullName(),
                o: {_id: 7},
                stmtId: 0,
            }],
            txnNumber: NumberLong(txnNumber++)
        }),
        ErrorCodes.FailedToParse,
        'doTxn should fail when inner transaction contains statement id.');

    // Malformed operation with unexpected field 'x'.
    assert.commandFailedWithCode(
        db.adminCommand({doTxn: [{op: 'i', ns: t.getFullName(), o: {_id: 0}, x: 1}]}),
        ErrorCodes.FailedToParse,
        'doTxn should fail on malformed operations.');

    assert.eq(0, t.find().count(), "Non-zero amount of documents in collection to start");

    /**
     * Test function for running CRUD operations on non-existent namespaces using various
     * combinations of invalid namespaces (collection/database)
     *
     * Leave 'expectedErrorCode' undefined if this command is expected to run successfully.
     */
    function testCrudOperationOnNonExistentNamespace(optype, o, o2, expectedErrorCode) {
        expectedErrorCode = expectedErrorCode || ErrorCodes.OK;
        const t2 = db.getSiblingDB('do_txn1_no_such_db').getCollection('t');
        [t, t2].forEach(coll => {
            const op = {op: optype, ns: coll.getFullName(), o: o, o2: o2};
            const cmd = {doTxn: [op], txnNumber: NumberLong(txnNumber++)};
            jsTestLog('Testing doTxn on non-existent namespace: ' + tojson(cmd));
            if (expectedErrorCode === ErrorCodes.OK) {
                assert.commandWorked(db.adminCommand(cmd));
            } else {
                assert.commandFailedWithCode(db.adminCommand(cmd), expectedErrorCode);
            }
        });
    }

    // Insert, delete, and update operations on non-existent collections/databases should return
    // NamespaceNotFound.
    testCrudOperationOnNonExistentNamespace('i', {_id: 0}, {}, ErrorCodes.NamespaceNotFound);
    testCrudOperationOnNonExistentNamespace('d', {_id: 0}, {}, ErrorCodes.NamespaceNotFound);
    testCrudOperationOnNonExistentNamespace('u', {x: 0}, {_id: 0}, ErrorCodes.NamespaceNotFound);

    assert.commandWorked(db.createCollection(t.getName()));
    var a = assert.commandWorked(db.adminCommand({
        doTxn: [{"op": "i", "ns": t.getFullName(), "o": {_id: 5, x: 17}}],
        txnNumber: NumberLong(txnNumber++)
    }));
    assert.eq(1, t.find().count(), "Valid insert failed");
    assert.eq(true, a.results[0], "Bad result value for valid insert");

    a = assert.commandWorked(db.adminCommand({
        doTxn: [{"op": "i", "ns": t.getFullName(), "o": {_id: 5, x: 17}}],
        txnNumber: NumberLong(txnNumber++)
    }));
    assert.eq(1, t.find().count(), "Duplicate insert failed");
    assert.eq(true, a.results[0], "Bad result value for duplicate insert");

    var o = {_id: 5, x: 17};
    assert.eq(o, t.findOne(), "Mismatching document inserted.");

    // 'o' field is an empty array.
    assert.commandFailed(
        db.adminCommand(
            {doTxn: [{op: 'i', ns: t.getFullName(), o: []}], txnNumber: NumberLong(txnNumber++)}),
        'doTxn should fail on insert of object with empty array element');

    var res = assert.commandWorked(db.runCommand({
        doTxn: [
            {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$set: {x: 18}}},
            {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$set: {x: 19}}}
        ],
        txnNumber: NumberLong(txnNumber++)
    }));

    o.x++;
    o.x++;

    assert.eq(1, t.find().count(), "Updates increased number of documents");
    assert.eq(o, t.findOne(), "Document doesn't match expected");
    assert.eq(true, res.results[0], "Bad result value for valid update");
    assert.eq(true, res.results[1], "Bad result value for valid update");

    // preCondition fully matches
    res = assert.commandWorked(db.runCommand({
        doTxn: [
            {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$set: {x: 20}}},
            {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$set: {x: 21}}}
        ],
        preCondition: [{ns: t.getFullName(), q: {_id: 5}, res: {x: 19}}],
        txnNumber: NumberLong(txnNumber++)
    }));

    o.x++;
    o.x++;

    assert.eq(1, t.find().count(), "Updates increased number of documents");
    assert.eq(o, t.findOne(), "Document doesn't match expected");
    assert.eq(true, res.results[0], "Bad result value for valid update");
    assert.eq(true, res.results[1], "Bad result value for valid update");

    // preCondition doesn't match ns
    res = assert.commandFailed(db.runCommand({
        doTxn: [
            {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$set: {x: 22}}},
            {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$set: {x: 23}}}
        ],
        preCondition: [{ns: "foo.otherName", q: {_id: 5}, res: {x: 21}}],
        txnNumber: NumberLong(txnNumber++)
    }));

    assert.eq(o, t.findOne(), "preCondition didn't match, but ops were still applied");

    // preCondition doesn't match query
    res = assert.commandFailed(db.runCommand({
        doTxn: [
            {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$set: {x: 22}}},
            {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$set: {x: 23}}}
        ],
        preCondition: [{ns: t.getFullName(), q: {_id: 5}, res: {x: 19}}],
        txnNumber: NumberLong(txnNumber++)
    }));

    assert.eq(o, t.findOne(), "preCondition didn't match, but ops were still applied");

    res = assert.commandFailed(db.runCommand({
        doTxn: [
            {op: "u", ns: t.getFullName(), o2: {_id: 5}, o: {$set: {x: 22}}},
            {op: "u", ns: t.getFullName(), o2: {_id: 6}, o: {$set: {x: 23}}}
        ],
        txnNumber: NumberLong(txnNumber++)
    }));

    assert.eq(false, res.results[0], "Op required upsert, which should be disallowed.");
    assert.eq(false, res.results[1], "Op required upsert, which should be disallowed.");

    // When applying a "u" (update) op, we default to 'UpdateNode' update semantics, and $set
    // operations add new fields in lexicographic order.
    res = assert.commandWorked(db.adminCommand({
        doTxn: [
            {"op": "i", "ns": t.getFullName(), "o": {_id: 6}},
            {"op": "u", "ns": t.getFullName(), "o2": {_id: 6}, "o": {$set: {z: 1, a: 2}}}
        ],
        txnNumber: NumberLong(txnNumber++)
    }));
    assert.eq(t.findOne({_id: 6}), {_id: 6, a: 2, z: 1});  // Note: 'a' and 'z' have been sorted.

    // 'ModifierInterface' semantics are not supported, so an update with {$v: 0} should fail.
    res = assert.commandFailed(db.adminCommand({
        doTxn: [
            {"op": "i", "ns": t.getFullName(), "o": {_id: 7}},
            {
              "op": "u",
              "ns": t.getFullName(),
              "o2": {_id: 7},
              "o": {$v: NumberLong(0), $set: {z: 1, a: 2}}
            }
        ],
        txnNumber: NumberLong(txnNumber++),
    }));
    assert.eq(res.code, 40682);

    // When we explicitly specify {$v: 1}, we should get 'UpdateNode' update semantics, and $set
    // operations get performed in lexicographic order.
    res = assert.commandWorked(db.adminCommand({
        doTxn: [
            {"op": "i", "ns": t.getFullName(), "o": {_id: 8}},
            {
              "op": "u",
              "ns": t.getFullName(),
              "o2": {_id: 8},
              "o": {$v: NumberLong(1), $set: {z: 1, a: 2}}
            }
        ],
        txnNumber: NumberLong(txnNumber++),
    }));
    assert.eq(t.findOne({_id: 8}), {_id: 8, a: 2, z: 1});  // Note: 'a' and 'z' have been sorted.
})();
