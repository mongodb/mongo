(function() {
'use strict';

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

while (true) {
    print("looping");
    sleep(1000);
}
})();
