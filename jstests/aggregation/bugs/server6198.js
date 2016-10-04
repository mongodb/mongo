// server-6198: disallow dots in group output field names
load('jstests/aggregation/extras/utils.js');

db.server6198.drop();

assertErrorCode(db.server6198, {$group: {_id: null, "bar.baz": {$addToSet: "$foo"}}}, 40235);
