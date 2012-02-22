// TestData is set by the smoketest framework; if it is not defined,
// then loaded files are assumed to be in in the current directory
if (typeof TestData !== "undefined" && typeof TestData.testPath !== "undefined") {
    var mydir = TestData.testPath.substr(0, TestData.testPath.lastIndexOf("/"))
    if (mydir[mydir.length - 1] !== '/') {
        mydir += '/';
    }
} else {
    var mydir = '';
}

// load utils
load(mydir + 'utils.js');

// test all the bug test cases
load(mydir + 'server3832.js');
load(mydir + 'server4508.js');
//load('server4638.js');
load(mydir + 'server4738.js');
load(mydir + 'server5012.js');
