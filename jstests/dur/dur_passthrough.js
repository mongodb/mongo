//
// simple runner to run toplevel tests in jstests
//

//TODO(mathias) use paranoid mode once we are reasonably sure it will pass

conn = startMongodEmpty("--port", 30100, "--dbpath", "/data/db/dur_passthrough", "--dur", "--smallfiles");
db = conn.getDB("test");

var files = listFiles("jstests");
files = files.sort(compareOn('name'));

var runnerStart = new Date()

function run(x) {
    if (/[\/\\]_/.test(x.name) ||
            !/\.js$/.test(x.name) ||
            /repair/.test(x.name) ||
            false // placeholder so all real tests end in ||
        ) {
        print("dur_passthrough.js >>>> skipping " + x.name);
        return;
    }

    print();
    print("dur_passthrough.js run " + x.name);
    print();
    print("         " + Date.timeFunc(function () { load(x.name); }, 1) + "ms");
}

// set this variable to debug by skipping to a specific test to start with and go from there
// var skippingTo = /cursora/;
var skippingTo = false;

files.forEach(
    function (x) {
        if (skippingTo && !skippingTo.test(x.name)) {
            print("dur_passthrough.js temp skip " + x.name);
            return;
        }
        skippingTo = false;
        try {
            run(x);
        }
        catch (e) {
            print("\n\n\n\ndur_passthrough.js FAIL " + x.name + "\n\n\n\n");
            throw e;
        }
    }
);

print("dur_passthrough.js stopMongod")
stopMongod(30100)

var runnerEnd = new Date()

print( "dur_passthrough.js total runner time: " + ( ( runnerEnd.getTime() - runnerStart.getTime() ) / 1000 ) + "secs" )
