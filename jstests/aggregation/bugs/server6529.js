// stop allowing field inclusion in objects in expressions
load('jstests/aggregation/extras/utils.js');

c = db.s6529;

c.drop();
c.save({a:{b:{c:{d:{e:{f:{g:19}}}}}}});

// bad project
assertErrorCode(c, {$project:{foo:{$add:[{b:1}]}}}, 16420);
// $group shouldnt allow numeric inclusions
assertErrorCode(c, {$group:{_id: {a:1}}}, 16420);

// but any amount of nesting in a project should work
assert.eq(c.aggregate({$project:{_id:0, a:{b:{c:{d:{e:{f:{g:1}}}}}}}}).result, [{a:{b:{c:{d:{e:{f:{g:19}}}}}}}]);
assert.eq(c.aggregate({$project:{_id:0, a:{b:{c:{d:{e:{f:1}}}}}}}).result, [{a:{b:{c:{d:{e:{f:{g:19}}}}}}}]);
assert.eq(c.aggregate({$project:{_id:0, a:{b:{c:{d:{e:1}}}}}}).result, [{a:{b:{c:{d:{e:{f:{g:19}}}}}}}]);
assert.eq(c.aggregate({$project:{_id:0, a:{b:{c:{d:1}}}}}).result, [{a:{b:{c:{d:{e:{f:{g:19}}}}}}}]);
assert.eq(c.aggregate({$project:{_id:0, a:{b:{c:1}}}}).result, [{a:{b:{c:{d:{e:{f:{g:19}}}}}}}]);
assert.eq(c.aggregate({$project:{_id:0, a:{b:1}}}).result, [{a:{b:{c:{d:{e:{f:{g:19}}}}}}}]);
assert.eq(c.aggregate({$project:{_id:0, a:1}}).result, [{a:{b:{c:{d:{e:{f:{g:19}}}}}}}]);
