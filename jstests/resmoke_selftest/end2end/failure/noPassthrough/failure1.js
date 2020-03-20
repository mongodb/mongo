(function() {
'use strict';

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

throw new Error("Explicitly forcing the JS test to fail.");
})();
