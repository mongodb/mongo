// server-6530: disallow $near queries in $match operations
load('jstests/aggregation/extras/utils.js');

assertErrorCode(db.foo, {$match: {$near: [0, 0]}}, 16424);
assertErrorCode(db.foo, {$match: {$nearSphere: [2, 2]}}, 16426);
