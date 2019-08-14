/**
 * Tests that we can create background (and foreground) indexes that are multikey.
 * @tags: [
 *  # Uses index building in background
 *  requires_background_index,
 * ]
 */

(function() {
"use strict";
function testIndexBuilds(isBackground) {
    jsTestLog("Testing " + (isBackground ? "background" : "foreground") + " index builds");
    let coll = db["background_index_multikey_" + isBackground];
    coll.drop();

    // Build index after multikey document is in the collection.
    let doc = {_id: 0, a: [1, 2]};
    assert.commandWorked(coll.insert(doc));
    assert.commandWorked(coll.createIndex({a: 1}, {background: isBackground}));
    assert.eq(1, coll.count({a: 1}));
    assert.eq(doc, coll.findOne({a: 1}));
    assert.eq(1, coll.count({a: 2}));
    assert.eq(doc, coll.findOne({a: 2}));

    // Build index where multikey is in an embedded document.
    doc = {_id: 1, b: {c: [1, 2]}};
    assert.commandWorked(coll.insert(doc));
    assert.commandWorked(coll.createIndex({'b.c': 1}, {background: isBackground}));
    assert.eq(1, coll.count({'b.c': 1}));
    assert.eq(doc, coll.findOne({'b.c': 1}));
    assert.eq(1, coll.count({'b.c': 2}));
    assert.eq(doc, coll.findOne({'b.c': 2}));

    // Add new multikey path to embedded path.
    doc = {_id: 2, b: [1, 2]};
    assert.commandWorked(coll.insert(doc));
    assert.eq(1, coll.count({b: 1}));
    assert.eq(doc, coll.findOne({b: 1}));
    assert.eq(1, coll.count({b: 2}));
    assert.eq(doc, coll.findOne({b: 2}));

    // Build index on a large collection that is not multikey, and then make it multikey.
    for (let i = 100; i < 1100; i++) {
        assert.commandWorked(coll.insert({_id: i, d: i}));
    }
    assert.commandWorked(coll.createIndex({d: 1}, {background: isBackground}));
    doc = {_id: 3, d: [1, 2]};
    assert.commandWorked(coll.insert(doc));
    assert.eq(1, coll.count({d: 1}));
    assert.eq(doc, coll.findOne({d: 1}));
    assert.eq(1, coll.count({d: 2}));
    assert.eq(doc, coll.findOne({d: 2}));

    // Build compound multikey index.
    doc = {_id: 4, e: [1, 2]};
    assert.commandWorked(coll.insert(doc));
    assert.commandWorked(coll.createIndex({'e': 1, 'f': 1}, {background: isBackground}));
    assert.eq(1, coll.count({e: 1}));
    assert.eq(doc, coll.findOne({e: 1}));
    assert.eq(1, coll.count({e: 2}));
    assert.eq(doc, coll.findOne({e: 2}));

    // Add new multikey path to compound index.
    doc = {_id: 5, f: [1, 2]};
    assert.commandWorked(coll.insert(doc));
    assert.eq(1, coll.count({f: 1}));
    assert.eq(doc, coll.findOne({f: 1}));
    assert.eq(1, coll.count({f: 2}));
    assert.eq(doc, coll.findOne({f: 2}));
}

testIndexBuilds(false);
testIndexBuilds(true);
})();
