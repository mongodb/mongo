
db.runCommand("forceerror");
assert.eq("forced error", db.getLastError());
db.runCommand("switchtoclienterrors");
assert.isnull(db.getLastError());
