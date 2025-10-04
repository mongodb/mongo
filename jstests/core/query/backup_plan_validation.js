// Validation test for SERVER-14753. Note that the issue under test is a memory leak, so this
// test would only be expected to fail when run under address sanitizer.

let t = db[jsTestName()];

t.drop();
t.createIndex({a: 1});
t.createIndex({b: 1});
for (var i = 0; i < 20; i++) {
    t.insert({b: i});
}
for (var i = 0; i < 20; i++) {
    t.find({b: 1}).sort({a: 1}).next();
}
