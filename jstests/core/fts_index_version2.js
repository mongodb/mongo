// Integration tests for version 2 text index, ensuring that it maintains old behavior.

load('jstests/libs/fts.js');

(function() {
    "use strict";
    var coll = db.fts_index_version2;

    coll.drop();

    assert.writeOK(coll.insert({
        _id: 0,
        a: "O próximo Vôo à Noite sobre o Atlântico, Põe Freqüentemente o único Médico."
    }));

    assert.commandWorked(
        coll.ensureIndex({a: "text"}, {default_language: "portuguese", textIndexVersion: 2}));

    assert.eq([0], queryIDS(coll, "próximo vôo à", null));
    assert.eq([0], queryIDS(coll, "atlântico", null));
    assert.eq([0], queryIDS(coll, "\"próxIMO\"", null));
    assert.eq([0], queryIDS(coll, "\"põe\" atlânTico", null));
    assert.eq([0], queryIDS(coll, "\"próximo vôo\" \"único médico\"", null));
    assert.eq([0], queryIDS(coll, "\"próximo vôo\" -\"único Atlântico\"", null));

    assert.eq([], queryIDS(coll, "proximo voo a", null));
    assert.eq([], queryIDS(coll, "átlántico", null));
    assert.eq([], queryIDS(coll, "\"proxIMO\"", null));
    assert.eq([], queryIDS(coll, "\"poé\" atlânTico", null));
    assert.eq([], queryIDS(coll, "\"próximo voo\" \"unico médico\"", null));
    assert.eq([], queryIDS(coll, "\"próximo Vôo\" -\"único Médico\"", null));

})();
