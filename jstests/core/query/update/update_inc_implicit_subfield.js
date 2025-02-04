// SERVER-3370 check modifiers with field name characters comparing less than '.' character.

const t = db[jsTestName()];

t.drop();
assert.commandWorked(t.update({}, {'$inc': {'all.t': 1, 'all-copy.t': 1}}, true));
assert.eq(1, t.count({all: {t: 1}, 'all-copy': {t: 1}}));

assert(t.drop());
assert.commandWorked(t.save({'all': {}, 'all-copy': {}}));
assert.commandWorked(t.update({}, {'$inc': {'all.t': 1, 'all-copy.t': 1}}));
assert.eq(1, t.count({all: {t: 1}, 'all-copy': {t: 1}}));

assert(t.drop());
assert.commandWorked(t.save({'all11': {}, 'all2': {}}));
assert.commandWorked(t.update({}, {'$inc': {'all11.t': 1, 'all2.t': 1}}));
assert.eq(1, t.count({all11: {t: 1}, 'all2': {t: 1}}));
