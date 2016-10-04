// Test replication of an array by $push-ing to a missing field in the presence of a sparse index on
// the field.  SERVER-4907

function testWithCollectionIndexIds(capped, sparse, useIds) {
    printjson({capped: capped, sparse: sparse, useIds: useIds});

    rt = new ReplTest("repl14tests");

    m = rt.start(true);  // master
    if (capped) {
        m.getDB('d').createCollection('c', {capped: true, size: 5 * 1024});
    }
    mc = m.getDB('d')['c'];  // master collection

    mc.ensureIndex({a: 1}, {sparse: sparse});
    toInsert = {};
    if (capped) {
        // Add a singleton array as padding, so the push later on will not change document size.
        toInsert = {p: [1]};
    }
    if (useIds) {  // Insert wiith an auto generated _id.
        mc.insert(toInsert);
    } else {  // Otherwise avoid the auto generated _id.
        mc._mongo.insert(mc._fullName, toInsert, 0);
    }
    assert.eq(mc.count(), 1);

    s = rt.start(false);     // slave
    sc = s.getDB('d')['c'];  // slave collection

    // Wait for the document to be cloned.
    assert.soon(function() {
        return sc.count() > 0;
    }, "doc not replicated soon enough", 60 * 1000);

    modifiers = {$push: {a: 1}};
    if (capped) {
        // Delete our singleton array to balance the new singleton array we're going to create.
        modifiers['$unset'] = {p: 1};
    }
    assert.writeOK(mc.update({}, modifiers));

    // Wait for the update to be replicated.
    assert.soon(function() {
        return sc.count({a: 1}) > 0;
    });

    rt.stop();
}

function testWithCollectionIndex(capped, sparse) {
    testWithCollectionIndexIds(capped, sparse, true);
    if (capped) {
        testWithCollectionIndexIds(capped, sparse, false);
    }
}

function testWithCollection(capped) {
    testWithCollectionIndex(capped, true);
    testWithCollectionIndex(capped, false);
}

function test() {
    testWithCollection(true);
    testWithCollection(false);
}

test();
