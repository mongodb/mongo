(function() {
    'use strict';

    var t = db.find_and_modify_server6226;
    t.drop();

    var ret = t.findAndModify({query: {_id: 1}, update: {"$inc": {i: 1}}, upsert: true});
    assert.isnull(ret);
})();
