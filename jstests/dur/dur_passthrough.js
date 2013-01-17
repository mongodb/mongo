//
// simple runner to run toplevel tests in jstests
//

//TODO(mathias) add --master or make another test
//conn = startMongodEmpty("--port", 30200, "--dbpath", "/data/db/dur_passthrough", "--dur", "--smallfiles", "--durOptions", "24");
conn = startMongodEmpty("--port", 30200, "--dbpath", "/data/db/dur_passthrough", "--dur", "--smallfiles", "--durOptions", "8");
db = conn.getDB("test");

var files = listFiles("jstests");
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

    }
);

stopMongod(30200);

var runnerEnd = new Date()

print( "total runner time: " + ( ( runnerEnd.getTime() - runnerStart.getTime() ) / 1000 ) + "secs" )

//TODO(mathias): test recovery here

