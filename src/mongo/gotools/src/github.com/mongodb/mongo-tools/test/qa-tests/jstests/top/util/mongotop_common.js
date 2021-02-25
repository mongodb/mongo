// mongotop_common.js; contains variables used by mongotop tests
/* exported executeProgram */
/* exported extractJSON */
load('jstests/common/topology_helper.js');
load('jstests/libs/extended_assert.js');

var executeProgram = function(args) {
  clearRawMongoProgramOutput();
  var pid = startMongoProgramNoConnect.apply(this, args);
  var exitCode = waitProgram(pid);
  var prefix = 'sh'+pid+'| ';
  var getOutput = function() {
    return rawMongoProgramOutput().split('\n').filter(function(line) {
      return line.indexOf(prefix) === 0;
    }).join('\n');
  };
  return {
    exitCode: exitCode,
    getOutput: getOutput,
  };
};

var extractJSON = function(shellOutput) {
  return shellOutput.substring(shellOutput.indexOf('{'), shellOutput.lastIndexOf('}') + 1);
};
