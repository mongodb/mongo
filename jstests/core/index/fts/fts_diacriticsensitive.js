// Integration tests for {$diacriticSensitive: true} option to $text query operator.

import {queryIDS} from "jstests/libs/fts.js";

let coll = db.fts_diacriticsensitive;

coll.drop();

assert.commandWorked(
    coll.insert({_id: 0, a: "O próximo vôo à noite sobre o Atlântico, põe freqüentemente o único médico."}),
);

assert.commandWorked(coll.createIndex({a: "text"}, {default_language: "portuguese"}));

assert.throws(function () {
    queryIDS(coll, "hello", null, {$diacriticSensitive: "invalid"});
});

assert.eq([0], queryIDS(coll, "PRÓXIMO VÔO À", null, {$diacriticSensitive: true}));
assert.eq([0], queryIDS(coll, "atlântico", null, {$diacriticSensitive: true}));
assert.eq([0], queryIDS(coll, '"próximo"', null, {$diacriticSensitive: true}));
assert.eq([0], queryIDS(coll, '"põe" atlântico', null, {$diacriticSensitive: true}));
assert.eq([0], queryIDS(coll, '"próximo vôo" "único médico"', null, {$diacriticSensitive: true}));
assert.eq([0], queryIDS(coll, '"próximo vôo" -"unico médico"', null, {$diacriticSensitive: true}));

assert.eq([], queryIDS(coll, "à", null, {$diacriticSensitive: true}));
assert.eq([], queryIDS(coll, "proximo", null, {$diacriticSensitive: true}));
assert.eq([], queryIDS(coll, "proximo voo à", null, {$diacriticSensitive: true}));
assert.eq([], queryIDS(coll, "à -PRÓXIMO -vôo", null, {$diacriticSensitive: true}));
assert.eq([], queryIDS(coll, "à proximo -vôo", null, {$diacriticSensitive: true}));
assert.eq([], queryIDS(coll, "mo vô", null, {$diacriticSensitive: true}));
assert.eq([], queryIDS(coll, '"unico medico"', null, {$diacriticSensitive: true}));
assert.eq([], queryIDS(coll, '"próximo vôo" -"único médico"', null, {$diacriticSensitive: true}));
