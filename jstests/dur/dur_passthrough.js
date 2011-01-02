// runs the toplevel jstests with --dur
// 
// TODO(mathias) use paranoid mode (--durOptions 8) once we are reasonably sure it will pass

// DEBUG : set this variable to debug by skipping to a specific test to start with and go from there
//var skippingTo = /null.js/;
var skippingTo = false;

conn = startMongodEmpty("--port", 30100, "--dbpath", "/data/db/dur_passthrough", "--dur", "--smallfiles");
db = conn.getDB("test");

function durPassThrough() {

    var runnerStart = new Date()

    var ran = {};

    /** run a test. won't run more than once. logs if fails and then throws.
    */
    function run(x) {
        function _run(x) {
            if (/[\/\\]_/.test(x.name) ||
                    !/\.js$/.test(x.name) ||
                    /repair/.test(x.name) ||
//		/numberlong/.test(x.name) ||
                    false // placeholder so all real tests end in ||
                ) {
                print("dur_passthrough.js >>>> skipping " + x.name);
                return;
            }
            print();
            print("dur_passthrough.js run " + x.name);
            print("dur_passthrough.js end " + x.name + ' ' + Date.timeFunc(function () { load(x.name); }, 1) + "ms");
            print();
        }
        if (ran[x.name])
            return;
        ran[x.name] = true;
        try {
            _run(x);
        }
        catch (e) {
            print("\n\n\n\ndur_passthrough.js FAIL " + x.name + "\n\n\n");
            throw e;
        }
    }

    var files = listFiles("jstests");

    if( !skippingTo ) {
	    // run something that will almost surely pass and is fast just to make sure our framework 
	    // here is really working
	    run({ name: 'jstests/basic1.js' });

	    // run "suspicious" tests early.  these are tests that have ever failed in buildbot.  we run them 
	    // early and try to get a fail fast
	    run({ name: 'jstests/shellstartparallel.js' });
	    run({ name: 'jstests/cursora.js' });

	    // run the shell-oriented tests early. if the shell is broken the other tests aren't meaningful
	    run({ name: 'jstests/run_program1.js' });
	    run({ name: 'jstests/shellspawn.js' });
	    run({ name: 'jstests/shellkillop.js' });
    }

    files = files.sort(compareOn('name'));
    files.forEach(
        function (x) {
            if (skippingTo && !skippingTo.test(x.name)) {
                print("dur_passthrough.js temp skip " + x.name);
                return;
            }
            skippingTo = false;
            run(x);
        }
    );

    print("dur_passthrough.js stopMongod");
    stopMongod(30100);
    var runnerEnd = new Date();
    print("dur_passthrough.js total runner time: " + ((runnerEnd.getTime() - runnerStart.getTime()) / 1000) + "secs")
}

durPassThrough();
