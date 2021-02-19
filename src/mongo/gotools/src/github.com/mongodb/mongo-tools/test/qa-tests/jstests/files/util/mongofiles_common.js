// mongofiles_common.js; contains variables used by mongofiles tests
load('jstests/common/topology_helper.js');

/* exported filesToInsert */
// these must have unique names
var filesToInsert = [
  'jstests/files/testdata/files1.txt',
  'jstests/files/testdata/files2.txt',
  'jstests/files/testdata/files3.txt'
];
