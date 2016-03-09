load('jstests/libs/command_line/test_parsed_options.js');

assert.docEq({x: 1, y: 1}, mergeOptions({x: 1}, {y: 1}));
assert.docEq({x: 1, y: 1}, mergeOptions({x: 1, y: 2}, {y: 1}));
assert.docEq({x: {z: 1}, y: 1}, mergeOptions({x: {z: 1}}, {y: 1}));
assert.docEq({x: {z: 1}}, mergeOptions({x: {z: 2}}, {x: {z: 1}}));
