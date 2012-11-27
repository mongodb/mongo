// SEVER-7768 aggregate cmd shouldn't fail when $readPreference is specified
collection = 'server7768';
db[collection].insert({foo:1});
res = db.runCommand({ 'aggregate': collection
                    , 'pipeline': [{'$project': {'_id': false, 'foo': true}}]
                    , $readPreference: {'mode': 'primary'}
                    });

assert.commandWorked(res);
assert.eq(res.result, [{foo:1}])
