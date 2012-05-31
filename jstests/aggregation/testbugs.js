
/* load the test documents */
load('jstests/aggregation/data/articles.js');

// load utils
load('jstests/aggregation/extras/utils.js');

// test all the bug test cases
load('jstests/aggregation/bugs/server3832.js');
load('jstests/aggregation/bugs/server4508.js');
//load('jstests/aggregation/bugs/server4638.js');
load('jstests/aggregation/bugs/server4738.js');
load('jstests/aggregation/bugs/server5012.js');
load('jstests/aggregation/bugs/server5209.js');
load('jstests/aggregation/bugs/server5369.js');
load('jstests/aggregation/bugs/server5973.js');
