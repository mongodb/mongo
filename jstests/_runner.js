//
// simple runner to run toplevel tests in jstests
//
var files = listFiles("jstests");

files.forEach(function(x) {

    if (!/_runner/.test(x.name)) { 
        print(" *******************************************");
        print("         Test : " + x.name + " ...");
        print("                " + Date.timeFunc( function() { load(x.name); }, 1) + "ms");
    }
    else { 
        print(" >>>>>>>>>>>>>>> skipping " + x.name);
    }
});


