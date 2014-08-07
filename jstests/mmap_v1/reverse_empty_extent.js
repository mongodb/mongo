// Test to make sure that a reverse cursor can correctly handle empty extents (SERVER-6980)

// Create a collection with three small extents
db.jstests_reversecursor.drop();
db.runCommand({"create":"jstests_reversecursor", $nExtents: [4096,4096,4096]});

// Function to check whether all three extents are non empty
function extentsSpanned() {
    var extents = db.jstests_reversecursor.validate(true).extents;
    return (extents[0].firstRecord != "null" &&
            extents[1].firstRecord != "null" &&
            extents[2].firstRecord != "null");
}

// Insert enough documents to span all three extents
a = 0;
while (!extentsSpanned()) {
    db.jstests_reversecursor.insert({a:a++});
}

// Delete all the elements in the middle
db.jstests_reversecursor.remove({a:{$gt:0,$lt:a-1}});

// Make sure the middle extent is empty and that both end extents are not empty
assert.eq(db.jstests_reversecursor.validate(true).extents[1].firstRecord, "null");
assert.eq(db.jstests_reversecursor.validate(true).extents[1].lastRecord, "null");
assert.neq(db.jstests_reversecursor.validate(true).extents[0].firstRecord, "null");
assert.neq(db.jstests_reversecursor.validate(true).extents[0].lastRecord, "null");
assert.neq(db.jstests_reversecursor.validate(true).extents[2].firstRecord, "null");
assert.neq(db.jstests_reversecursor.validate(true).extents[2].lastRecord, "null");

// Make sure that we get the same number of elements for both the forward and reverse cursors
assert.eq(db.jstests_reversecursor.find().sort({$natural:1}).toArray().length, 2);
assert.eq(db.jstests_reversecursor.find().sort({$natural:-1}).toArray().length, 2);
