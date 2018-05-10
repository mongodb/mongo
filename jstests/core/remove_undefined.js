// @tags: [requires_non_retryable_writes, requires_fastcount]

(function() {
    "use strict";

    const coll = db.remove_undefined;
    coll.drop();

    assert.writeOK(coll.insert({_id: 1}));
    assert.writeOK(coll.insert({_id: 2}));
    assert.writeOK(coll.insert({_id: null}));

    const obj = {foo: 1, nullElem: null};

    coll.remove({x: obj.bar});
    assert.eq(3, coll.count());

    coll.remove({x: undefined});
    assert.eq(3, coll.count());

    assert.writeErrorWithCode(coll.remove({_id: obj.bar}), ErrorCodes.BadValue);
    assert.writeErrorWithCode(coll.remove({_id: undefined}), ErrorCodes.BadValue);

    coll.remove({_id: obj.nullElem});
    assert.eq(2, coll.count());

    assert.writeOK(coll.insert({_id: null}));
    assert.eq(3, coll.count());

    assert.writeErrorWithCode(coll.remove({_id: undefined}), ErrorCodes.BadValue);
    assert.eq(3, coll.count());
})();
