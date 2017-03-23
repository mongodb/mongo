(function() {
    'use strict';
    var t = db.group_owned;
    t.drop();

    assert.writeOK(t.insert({_id: 1, subdoc: {id: 1}}));
    assert.writeOK(t.insert({_id: 2, subdoc: {id: 2}}));

    var result = t.group({
        key: {'subdoc.id': 1},
        reduce: function(doc, value) {
            value.subdoc = doc.subdoc;
            return value;
        },
        initial: {},
        finalize: function(res) {}
    });

    assert(result.length == 2);
}());
