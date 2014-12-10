// mongotop_common.js; contains variables used by mongotop tests
//
load('jstests/common/topology_helper.js');

var shellOutputRegex = '^sh.*';

var extractJSON = function(shellOutput) {
  return shellOutput.substring(shellOutput.indexOf('{'), shellOutput.lastIndexOf('}') + 1);
}
