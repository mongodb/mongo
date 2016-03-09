// SEVER-7768 aggregate cmd shouldn't fail when $readPreference is specified
collection = 'server7768';
db[collection].drop();
db[collection].insert({foo: 1});
// Can't use aggregate helper here because we need to add $readPreference flag
res = db.runCommand({
    'aggregate': collection,
    'pipeline': [{'$project': {'_id': false, 'foo': true}}],
    $readPreference: {'mode': 'primary'}
});

assert.commandWorked(res);
assert.eq(res.result, [{foo: 1}]);
