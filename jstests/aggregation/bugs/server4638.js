// SERVER-4638 - this tests explicit undefined values
// This case is marked as a dup of SERVER-4674

t = db.server4638;
t.drop();

t.insert({_id: 0, x: 0, undef: undefined});

// Make sure having an undefined doesn't break pipelines not using the field
res = t.aggregate({$project: {x: 1}}).toArray();
assert.eq(res[0].x, 0);

// Make sure having an undefined doesn't break pipelines that do use the field
res = t.aggregate({$project: {undef: 1}}).toArray();
assert.eq(res[0].undef, undefined);
assert.eq(typeof(res[0].undef), "undefined");
