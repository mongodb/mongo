
var ports = [40001, 40002, 40011, 40012, 40013, 40021, 40022, 40023, 40041, 40101, 40102, 40103, 40201, 40202, 40203]
var auth = [40002, 40103, 40203, 40031]

for (var i in ports) {
    var port = ports[i]
    var server = "localhost:" + port
    var mongo = new Mongo("localhost:" + port)
    var admin = mongo.getDB("admin")
    
    for (var j in auth) {
        if (auth[j] == port) {
            admin.auth("root", "rapadura")
            break
        }
    }
    var result = admin.runCommand({"listDatabases": 1})
    // Why is the command returning undefined!?
    while (typeof result.databases == "undefined") {
        print("dropall.js: listing databases of :" + port + " got:", result)
        result = admin.runCommand({"listDatabases": 1})
    }
    var dbs = result.databases
    for (var j = 0; j != dbs.length; j++) {
        var db = dbs[j]
        switch (db.name) {
        case "admin":
        case "local":
        case "config":
            break
        default:
            mongo.getDB(db.name).dropDatabase()
        }
    }
}

// vim:ts=4:sw=4:et
