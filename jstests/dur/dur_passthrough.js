//
// simple runner to run toplevel tests in jstests
//

//TODO(mathias) use paranoid mode once we are reasonably sure it will pass

conn = startMongodEmpty("--port", 30000, "--dbpath", "/data/db/dur_passthrough", "--dur", "--smallfiles");
db = conn.getDB("test");

var files = listFiles("jstests");
files = files.sort(compareOn('name'));

var runnerStart = new Date()

files.forEach(
    function (x) {

        if (/[\/\\]_/.test(x.name) ||
             !/\.js$/.test(x.name)) {
            print(" >>>>>>>>>>>>>>> skipping " + x.name);
            return;
        }

        print();
        print(" *******************************************");
        print("         Test : " + x.name + " ...");
        print("                " + Date.timeFunc(function () { load(x.name); }, 1) + "ms");

    }
);

stopMongod(30000);

var runnerEnd = new Date()

print( "total runner time: " + ( ( runnerEnd.getTime() - runnerStart.getTime() ) / 1000 ) + "secs" )
