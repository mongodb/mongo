// test all the bug test cases
var files = listFiles("jstests/aggregation/bugs");
files.forEach(
    function(x) {
        // skip files starting with _ and only test *.js files
        if (/[\/\\]_/.test(x.name) || ! /\.js$/.test(x.name ) ) {
            print(" >>>>>>>>>>>>>>> skipping " + x.name);
            return;
        }

        // clean out the test documents
        db.article.drop();

        print(" *******************************************");
        print("         Test : " + x.name + " ...");
        print("                " + Date.timeFunc(function(){ load(x.name); }, 1) + "ms");
    }
);
