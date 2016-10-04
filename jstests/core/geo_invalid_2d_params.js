var t = db.geo_invalid_2d_params;
t.drop();

assert.commandFailed(t.ensureIndex({loc: "2d"}, {bits: 33}));
assert.commandFailed(t.ensureIndex({loc: "2d"}, {min: -1, max: -1}));
assert.commandFailed(t.ensureIndex({loc: "2d"}, {bits: -1}));
assert.commandFailed(t.ensureIndex({loc: "2d"}, {min: 10, max: 9}));
assert.commandWorked(t.ensureIndex({loc: "2d"}, {bits: 1, min: -1, max: 1}));