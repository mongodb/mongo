// just make sure logout doesn't break anything

// SERVER-724
db.runCommand({logout : 1});
db.runCommand({logout : 1});
