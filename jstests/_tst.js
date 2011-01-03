/* a general testing framework (helpers) for us in the jstests/

   to use, from your test file:
     testname="mytestname";
     load("jstests/_tst.js");
*/

if( typeof tst == "undefined" ) {
    tst = {}

    tst.log = function (optional_msg) {
        print("\n\nstep " + ++this._step + " " + (optional_msg || ""));
    }

    tst.success = function () {
        print(testname + " SUCCESS");
    }

    /* diff files a and b, returning the difference (empty str if no difference) */
    tst.diff = function(a, b) {
        function reSlash(s) {
            var x = s;
            if (_isWindows()) {
                while (1) {
                    var y = x.replace('/', '\\');
                    if (y == x)
                        break;
                    x = y;
                }
            }
            return x;
        }
        a = reSlash(a);
        b = reSlash(b);
        print("diff " + a + " " + b);
        return run("diff", a, b);
    }
}   

print(testname + " BEGIN");
tst._step = 0;
