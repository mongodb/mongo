//var settings = {heartbeatSleep: 0.05, heartbeatTimeout: 0.5}
var settings = {};

// We know the master of the first set (pri=1), but not of the second.
var rs1cfg = {_id: "rs1",
              members: [{_id: 1, host: "127.0.0.1:40011", priority: 1, tags: {rs1: "a"}},
                        {_id: 2, host: "127.0.0.1:40012", priority: 0, tags: {rs1: "b"}},
                        {_id: 3, host: "127.0.0.1:40013", priority: 0, tags: {rs1: "c"}}],
              settings: settings}
var rs2cfg = {_id: "rs2",
              members: [{_id: 1, host: "127.0.0.1:40021", priority: 1, tags: {rs2: "a"}},
                        {_id: 2, host: "127.0.0.1:40022", priority: 1, tags: {rs2: "b"}},
                        {_id: 3, host: "127.0.0.1:40023", priority: 1, tags: {rs2: "c"}}],
              settings: settings}
var rs3cfg = {_id: "rs3",
              members: [{_id: 1, host: "127.0.0.1:40031", priority: 1, tags: {rs3: "a"}},
                        {_id: 2, host: "127.0.0.1:40032", priority: 1, tags: {rs3: "b"}},
                        {_id: 3, host: "127.0.0.1:40033", priority: 1, tags: {rs3: "c"}}],
              settings: settings}

// rs4 is left unconfigured for a test connecting to unconfigured rs members

var rs5cfg = {_id: "rs5",
              members: [{_id: 1, host: "127.0.0.1:40051", priority: 1}],
              settings: settings}

// For 3.4 CSRS, make each config server into a single-server RS
var cfg1cfg = {_id: "csrs1",
              configsvr: true,
              members: [{_id: 1, host: "127.0.0.1:40101", priority: 1}],
              settings: settings}
var cfg2cfg = {_id: "csrs2",
              configsvr: true,
              members: [{_id: 1, host: "127.0.0.1:40102", priority: 1}],
              settings: settings}
var cfg3cfg = {_id: "csrs3",
              configsvr: true,
              members: [{_id: 1, host: "127.0.0.1:40103", priority: 1}],
              settings: settings}

for (var i = 0; i != 60; i++) {
	try {
		db1 = new Mongo("127.0.0.1:40001").getDB("admin")
		db2 = new Mongo("127.0.0.1:40002").getDB("admin")
		rs1a = new Mongo("127.0.0.1:40011").getDB("admin")
		rs2a = new Mongo("127.0.0.1:40021").getDB("admin")
		rs3a = new Mongo("127.0.0.1:40031").getDB("admin")
		rs5a = new Mongo("127.0.0.1:40051").getDB("admin")
		cfg1 = new Mongo("127.0.0.1:40101").getDB("admin")
		cfg2 = new Mongo("127.0.0.1:40102").getDB("admin")
		cfg3 = new Mongo("127.0.0.1:40103").getDB("admin")
		break
	} catch(err) {
		print("Can't connect yet...")
	}
	sleep(1000)
}

function hasSSL() {
    return Boolean(db1.serverBuildInfo().OpenSSLVersion)
}

function tryRunCommand(db,cmd) {
    var r = db.runCommand(cmd)
    if (r.ok) {
        return r
    }
    print("Command failed: ", JSON.stringify(cmd))
    print("Result: ", JSON.stringify(r))
    quit(1)
}

print("Initiating rs1")
tryRunCommand(rs1a, {replSetInitiate: rs1cfg})
print("Initiating rs2")
tryRunCommand(rs2a, {replSetInitiate: rs2cfg})
print("Initiating rs3")
tryRunCommand(rs3a, {replSetInitiate: rs3cfg})
print("Initiating rs5")
tryRunCommand(rs5a, {replSetInitiate: rs5cfg})

ver = db1.serverBuildInfo().versionArray
if (ver > [3, 3]) {
    print("Initiating cfg1")
    tryRunCommand(cfg1, {replSetInitiate: cfg1cfg})
    print("Initiating cfg2")
    tryRunCommand(cfg2, {replSetInitiate: cfg2cfg})
    print("Initiating cfg3")
    tryRunCommand(cfg3, {replSetInitiate: cfg3cfg})
}

function configShards() {
    print("Configuring cluster 1")
    s1 = new Mongo("127.0.0.1:40201").getDB("admin")
    tryRunCommand(s1, {addshard: "rs1/127.0.0.1:40011"})
    tryRunCommand(s1, {addshard: "rs5/127.0.0.1:40051"})

    print("Configuring cluster 2")
    s2 = new Mongo("127.0.0.1:40202").getDB("admin")
    tryRunCommand(s2, {addshard: "rs2/127.0.0.1:40021"})

    print("Configuring cluster 3")
    s3 = new Mongo("127.0.0.1:40203").getDB("admin")
    tryRunCommand(s3, {addshard: "rs3/127.0.0.1:40031"})
}

function configAuth(addr) {
    print("Configuring auth for", addr)
    var db = new Mongo(addr).getDB("admin")
    var v = db.serverBuildInfo().versionArray
    var timedOut = false
    if (v < [2, 5]) {
        db.addUser("root", "rapadura")
    } else {
        try {
            db.createUser({user: "root", pwd: "rapadura", roles: ["root"]})
        } catch (err) {
            // 3.2 consistently fails replication of creds on 40031 (config server) 
            print("createUser command returned an error: " + err)
            if (String(err).indexOf("timed out") >= 0) {
                timedOut = true;
            }
        }
    }
    for (var i = 0; i < 60; i++) {
        var ok = db.auth("root", "rapadura")
        if (ok || !timedOut) {
            break
        }
        sleep(1000);
    }
    if (v >= [2, 6]) {
        db.createUser({user: "reader", pwd: "rapadura", roles: ["readAnyDatabase"]})
    } else if (v >= [2, 4]) {
        db.addUser({user: "reader", pwd: "rapadura", roles: ["readAnyDatabase"]})
    } else {
        db.addUser("reader", "rapadura", true)
    }
    if (v > [3, 7] && addr == "127.0.0.1:40002") {
        // username IX is used for SCRAM-SHA-256 so we can test saslprep on it
        db.createUser({user:"IX", pwd:"IX", mechanisms: ["SCRAM-SHA-256"], roles: ["readAnyDatabase"]})
        db.createUser({user:"\u2168", pwd:"\u2163", mechanisms: ["SCRAM-SHA-256"], roles: ["readAnyDatabase"]})
        db.createUser({user:"sha1", pwd:"sha1", mechanisms: ["SCRAM-SHA-1"], roles: ["readAnyDatabase"]})
        db.createUser({user:"both", pwd:"both", mechanisms: ["SCRAM-SHA-1", "SCRAM-SHA-256"], roles: ["readAnyDatabase"]})
    }
}

function countHealthy(rs) {
    var status = rs.runCommand({replSetGetStatus: 1})
    var count = 0
    var primary = 0
    if (typeof status.members != "undefined") {
        for (var i = 0; i != status.members.length; i++) {
            var m = status.members[i]
            if (m.health == 1 && (m.state == 1 || m.state == 2)) {
                count += 1
                if (m.state == 1) {
                    primary = 1
                }
            }
        }
    }
    if (primary == 0) {
	    count = 0
    }
    return count
}

var totalRSMembers = rs1cfg.members.length + rs2cfg.members.length + rs3cfg.members.length + rs5cfg.members.length

if (ver > [3, 3]) {
    totalRSMembers += 3
}

for (var i = 0; i != 60; i++) {
    var count = countHealthy(rs1a) + countHealthy(rs2a) + countHealthy(rs3a) + countHealthy(rs5a)
    if (ver > [3, 3]) {
        count += countHealthy(cfg1) + countHealthy(cfg2) + countHealthy(cfg3)
    }
    print("Replica sets have", count, "healthy nodes.")
    if (count == totalRSMembers) {
        print("Sleeping to let replicas settle")
        sleep(2000)
        print("Configuring non-shard auth")
        configAuth("127.0.0.1:40002")
        configAuth("127.0.0.1:40031")
        if (hasSSL()) {
            configAuth("127.0.0.1:40003")
        }
        configShards()
        print("Sleeping to let shards settle")
        sleep(2000)
        configAuth("127.0.0.1:40203")
        quit(0)
    }
    sleep(1000)
}

print("Replica sets didn't sync up properly.")
quit(12)

// vim:ts=4:sw=4:et
