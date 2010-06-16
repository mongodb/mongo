// test helpers
// load("test_framework.js")

DB.prototype.isMaster = function() { 
    return this.runCommand("isMaster");
}
DB.prototype.ismaster = function () { return this.isMaster().ismaster; }

function rs_mongod() {
    /* run mongod for a replica set member. wipes data dir! */
    var port = __nextPort++;
    var not_me = (port == 27000 ? port + 1 : port - 1);
    var f = startMongodEmpty;
    var dir = "" + port; // e.g., data/db/27000
    var conn = f.apply(null, [
        {
            port: port,
            dbpath: "/data/db/" + dir,
            noprealloc: "",
            smallfiles: "",
            oplogSize: "2",
            //nohttpinterface: ""
            rest: "", // --rest is best for replica set administration
            replSet: "asdf/" + hostname() + ":" + not_me
        }
    ]
    );
    conn.name = "localhost:" + port;
    return conn;
}
