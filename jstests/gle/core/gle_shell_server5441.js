
// Call the defaultPrompt function once at the beginning of the test to cache the
// build info. The first invocation of the function would otherwise overwrite the
// getLastError result.
defaultPrompt();

t = db.server5441;
t.drop();

function checkgle(iteration) {
    var gle = db.getLastErrorObj();
    assert.eq(2, gle.n, "failed on iteration " + iteration + ", getLastErrorObj()=" + tojson(gle));
}

t.insert({x: 1});
t.insert({x: 1});
updateReturn = t.update({}, {$inc: {x: 2}}, false, true);

for (i = 0; i < 100; i++) {
    checkgle("" + i);
}

db.adminCommand({replSetGetStatus: 1, forShell: 1});
shellPrintHelper(updateReturn);
defaultPrompt();

checkgle("'final'");
