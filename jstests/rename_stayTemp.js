orig = 'rename_stayTemp_orig'
dest = 'rename_stayTemp_dest'

db[orig].drop()
db[dest].drop()

function ns(coll){ return db[coll].getFullName() }

db.runCommand({create: orig, temp:1})
assert.eq(db.system.namespaces.findOne({name:ns(orig)}).options.temp, 1)

db.adminCommand({renameCollection: ns(orig), to: ns(dest)});
var options = db.system.namespaces.findOne({name:ns(dest)}).options || {};
assert.eq(options.temp, undefined);

db[dest].drop();

db.runCommand({create: orig, temp:1})
assert.eq(db.system.namespaces.findOne({name:ns(orig)}).options.temp, 1)

db.adminCommand({renameCollection: ns(orig), to: ns(dest), stayTemp: true});
assert.eq(db.system.namespaces.findOne({name:ns(dest)}).options.temp, 1)


