// SERVER-16497
var t=db.index_bigkeys_nofail;
t.drop();
var res=db.getSiblingDB('admin').runCommand( { setParameter: 1, failIndexKeyTooLong: true } );
var was=res.was;
assert.commandWorked(res);


var x = new Array(1025).join('x');
assert.commandWorked(t.ensureIndex({name:1}));
assert.writeError(t.insert({name:x}));
assert.commandWorked(t.dropIndex({name:1}));
assert.writeOK(t.insert({name:x}));
assert.commandFailed(t.ensureIndex({name:1}));

t.drop();
db.getSiblingDB('admin').runCommand( { setParameter: 1, failIndexKeyTooLong: false } );
assert.writeOK(t.insert({name:x}));
assert.commandWorked(t.ensureIndex({name:1}));
assert.writeOK(t.insert({name:x}));

db.getSiblingDB('admin').runCommand( { setParameter: 1, failIndexKeyTooLong: was } );
