/**
 * 1. check top numbers are correct
 */

var name = "toptest";

var testDB = db.getSiblingDB(name);
var testColl = testDB[name + "coll"];
testColl.drop();

// Perform an operation on the collection so that it is present in the "top" command's output.
assert.eq(testColl.find({}).itcount(), 0);

// get top statistics for the test collection
function getTop() {
    return testDB.adminCommand("top").totals[testColl.getFullName()];
}

//  This variable is used to get differential output
var lastTop = getTop();

//  return the number of operations since the last call to diffTop for the specified key
function diffTop(key) {
    var thisTop = getTop();
    var difference = {
        time: thisTop[key].time - lastTop[key].time,
        count: thisTop[key].count - lastTop[key].count
    };
    lastTop[key] = thisTop[key];

    assert.gte(difference.count, 0, "non-decreasing count");
    assert.gte(difference.time, 0, "non-decreasing time");

    //  Time should advance iff operations were performed
    assert.eq(difference.count !== 0, difference.time > 0, "non-zero time iff non-zero count");
    return difference;
}

var numRecords = 100;

// check stats for specified key are as expected
var checked = {};
function checkStats(key, expected) {
    checked[key]++;
    var actual = diffTop(key).count;
    assert.eq(actual, expected, "top reports wrong count for " + key);
}

//  Insert
for (var i = 0; i < numRecords; i++) {
    assert.writeOK(testColl.insert({_id: i}));
}
checkStats("insert", numRecords);
checkStats("writeLock", numRecords);

// Update
for (i = 0; i < numRecords; i++) {
    assert.writeOK(testColl.update({_id: i}, {x: i}));
}
checkStats("update", numRecords);

// Queries
var query = {};
for (i = 0; i < numRecords; i++) {
    query[i] = testColl.find({x: {$gte: i}}).batchSize(2);
    assert.eq(query[i].next()._id, i);
}
checkStats("queries", numRecords);

// Getmore
for (i = 0; i < numRecords / 2; i++) {
    assert.eq(query[i].next()._id, i + 1);
    assert.eq(query[i].next()._id, i + 2);
    assert.eq(query[i].next()._id, i + 3);
    assert.eq(query[i].next()._id, i + 4);
}
checkStats("getmore", numRecords);

// Remove
for (i = 0; i < numRecords; i++) {
    assert.writeOK(testColl.remove({_id: 1}));
}
checkStats("remove", numRecords);

// Upsert, note that these are counted as updates, not inserts
for (i = 0; i < numRecords; i++) {
    assert.writeOK(testColl.update({_id: i}, {x: i}, {upsert: 1}));
}
checkStats("update", numRecords);

// Commands
var res;

// "count" command
diffTop("commands");  // ignore any commands before this
for (i = 0; i < numRecords; i++) {
    res = assert.commandWorked(testDB.runCommand({count: testColl.getName()}));
    assert.eq(res.n, numRecords, tojson(res));
}
checkStats("commands", numRecords);

// "findAndModify" command
diffTop("commands");
for (i = 0; i < numRecords; i++) {
    res = assert.commandWorked(testDB.runCommand({
        findAndModify: testColl.getName(),
        query: {_id: i},
        update: {$inc: {x: 1}},
    }));
    assert.eq(res.value.x, i, tojson(res));
}
checkStats("commands", numRecords);

diffTop("commands");
for (i = 0; i < numRecords; i++) {
    res = assert.commandWorked(testDB.runCommand({
        findAndModify: testColl.getName(),
        query: {_id: i},
        remove: true,
    }));
    assert.eq(res.value.x, i + 1, tojson(res));
}
checkStats("commands", numRecords);

for (var key of Object.keys(lastTop)) {
    if (!checked.hasOwnProperty(key)) {
        printjson({key: key, stats: diffTop(key)});
    }
}
