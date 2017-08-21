// server-6530: disallow $near queries in $match operations
load('jstests/aggregation/extras/utils.js');

assertErrorCode(db.foo, {$match: {$near: [0, 0]}}, ErrorCodes.BadValue);
assertErrorCode(db.foo, {$match: {$nearSphere: [2, 2]}}, ErrorCodes.BadValue);
assertErrorCode(db.foo, {$match: {$geoNear: [2, 2]}}, ErrorCodes.BadValue);
