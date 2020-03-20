(function() {
'use strict';

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

// Loop infinitely to simulate timeout.
while (true) {
    print('looping...');
    sleep(100);
}
})();
