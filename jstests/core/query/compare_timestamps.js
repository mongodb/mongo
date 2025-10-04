// SERVER-21160: Check that timestamp comparisons are unsigned
let t = db.compare_timestamps;
t.drop();
assert.commandWorked(t.insert({a: new Timestamp(0xffffffff, 3), b: "non-zero"}));
assert.commandWorked(t.insert({a: new Timestamp(0, 0), b: "zero"}));
assert.eq(t.find().sort({a: 1}).limit(1).next().b, "zero", "timestamp must compare unsigned");
