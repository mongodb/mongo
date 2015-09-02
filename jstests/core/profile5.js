// Basic test to ensure that the keysExamined and docsExamined are tracked
// correctly for updates.

var t = db.jstests_update_profile;
t.drop();

// Turn off profiling so that we can drop the profiler's collection. Then
// enable profiling again. This is OK because this test is blacklisted for the
// parallel suite.
db.setProfilingLevel(0);
db.system.profile.drop();
db.setProfilingLevel(2);

for (var i = 0; i < 5; i++) {
    t.insert({x: i});
}

// No index---this update will do a collection scan.
t.update({x: {$gt: 3}}, {$set: {y: true}}, {multi: true});

printjson(t.find().toArray());

assert.eq(1, db.system.profile.count({op: "update"}),
          "expected exactly one update op in system.profile");
var prof = db.system.profile.findOne({op: "update"});
printjson(prof);

// Since we're doing a collection scan, we should have examined zero
// index keys and all 5 documents.
assert.eq(0, prof.keysExamined, "wrong keysExamined");
assert.eq(5, prof.docsExamined, "wrong docsExamined");

// Disable profiling and drop the profiler data.
db.setProfilingLevel(0);
db.system.profile.drop();
