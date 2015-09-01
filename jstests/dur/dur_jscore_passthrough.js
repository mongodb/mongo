//
// simple runner to run toplevel tests in jstests
//

var conn = MongoRunner.runMongod({journal: "", nopreallocj: "", smallfiles: "", journalOptions: 8});
db = conn.getDB("test");
conn.forceWriteMode("commands");

function doTest() {
    "use strict"

    // Some tests use "conn" and can override the "conn" defined above, so save a copy of it here
    // in local scope.
    var myConn = conn.port;

    var files = listFiles("jstests/core");
    files = files.sort(compareOn('name'));

    var runnerStart = new Date()

    files.forEach(
        function (x) {

            if (/[\/\\]_/.test(x.name) ||
                 !/\.js$/.test(x.name) ||
                 /repair/.test(x.name) || // fails on recovery
                 /shellkillop/.test(x.name) || // takes forever and don't test anything new
                 false // placeholder so all real tests end in ||
               )
            {
                print(" >>>>>>>>>>>>>>> skipping " + x.name);
                return;
            }

            print();
            print(" *******************************************");
            print("         Test : " + x.name + " ...");
            print("                " + Date.timeFunc(function () { load(x.name); }, 1) + "ms");
            gc(); // TODO SERVER-8683: remove gc() calls once resolved
        }
    );

    MongoRunner.stopMongod(myConn);

    var runnerEnd = new Date()

    print( "total runner time: " + ( ( runnerEnd.getTime() - runnerStart.getTime() ) / 1000 ) + "secs" )
}

if (db.serverBuildInfo().bits == 64 &&
    db.serverBuildInfo().debug == false &&
    db.hostInfo().os.type == "Linux")
{
        doTest();
}
else {
    print("Skipping.  Only run this test on non-debug, 64bit, Linux builds");
}

//TODO(mathias): test recovery here

