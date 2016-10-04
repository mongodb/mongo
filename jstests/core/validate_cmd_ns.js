/**
 * Tests that query against the $cmd namespace will error out when the request has
 * a number to return value other than 1 or -1. Note that users cannot have
 * collections named $cmd since $ is an illegal character.
 */

// Note: _exec gives you the raw response from the server.
var res = db.$cmd.find({whatsmyuri: 1})._exec().next();
assert.commandFailed(res);
assert(res.errmsg.indexOf('bad numberToReturn') > -1);

res = db.$cmd.find({whatsmyuri: 1}).limit(0)._exec().next();
assert.commandFailed(res);
assert(res.errmsg.indexOf('bad numberToReturn') > -1);

res = db.$cmd.find({whatsmyuri: 1}).limit(-2)._exec().next();
assert.commandFailed(res);
assert(res.errmsg.indexOf('bad numberToReturn') > -1);

res = db.$cmd.find({whatsmyuri: 1}).limit(1).next();
assert.commandWorked(res);

res = db.$cmd.find({whatsmyuri: 1}).limit(-1).next();
assert.commandWorked(res);
