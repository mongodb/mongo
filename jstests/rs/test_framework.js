// test helpers
// load("test_framework.js")

/* run mongod for a replica set member
   wipes data dir! 
*/
function rs_mongod() {
    var port = __nextPort++;
    var not_me = (port == 27000 ? port+1 : port-1);
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
            rest: "", 
            replSet: "asdf/" + hostname() + ":" + not_me
        }
    ]
    );
    conn.name = "localhost:" + port;
    return conn;
}


