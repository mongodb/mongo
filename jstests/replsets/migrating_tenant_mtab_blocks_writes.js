/**
 *
 * @tags: [requires_fcv_46]
 */

(function () {
    "use strict";


    const rst = new ReplSetTest({ nodes: 1 });
    rst.startSet();
    rst.initiate();

    assert(true)

    rst.stopSet();
})();