// SERVER-15810: Server crash when running a poorly formed command
var res = db.runCommand({aggregate: 1, pipeline: []});
assert.commandFailed(res); // command must fail
assert(!('code' in res));  // but must not cause massert
