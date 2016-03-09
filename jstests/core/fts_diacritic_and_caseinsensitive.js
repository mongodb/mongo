// Integration tests for no case or diacritic options to $text query operator.

load('jstests/libs/fts.js');

(function() {
    "use strict";
    var coll = db.fts_diacritic_and_caseinsensitive;

    coll.drop();

    assert.writeOK(coll.insert({
        _id: 0,
        a: "O próximo Vôo à Noite sobre o Atlântico, Põe Freqüentemente o único Médico."
    }));

    assert.commandWorked(coll.ensureIndex({a: "text"}, {default_language: "portuguese"}));

    assert.eq([0], queryIDS(coll, "proximo voo a", null));
    assert.eq([0], queryIDS(coll, "átlántico", null));
    assert.eq([0], queryIDS(coll, "\"proxIMO\"", null));
    assert.eq([0], queryIDS(coll, "\"poé\" atlânTico", null));
    assert.eq([0], queryIDS(coll, "\"próximo voo\" \"unico médico\"", null));
    assert.eq([0], queryIDS(coll, "\"proximo voo\" -\"unico atlantico\"", null));

    assert.eq([], queryIDS(coll, "À", null));
    assert.eq([], queryIDS(coll, "próximoo", null));
    assert.eq([], queryIDS(coll, "proximoo vvôo àa", null));
    assert.eq([], queryIDS(coll, "À -próximo -Vôo", null));
    assert.eq([], queryIDS(coll, "à proximo -voo", null));
    assert.eq([], queryIDS(coll, "mo vo", null));
    assert.eq([], queryIDS(coll, "\"unico atlantico\"", null));
    assert.eq([], queryIDS(coll, "\"próximo Vôo\" -\"unico medico\"", null));

})();
