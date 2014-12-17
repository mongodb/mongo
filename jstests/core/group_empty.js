
t = db.group_empty;
t.drop();

res1 = db.runCommand({group: {$reduce: function(){}, ns: 'group_empty', cond: {}, key: {}, initial: {count: 0}}});
t.ensureIndex( { x : 1 } );
res2 = db.runCommand({group: {$reduce: function(){}, ns: 'group_empty', cond: {}, key: {}, initial: {count: 0}}});
assert.eq( res1, res2 );
