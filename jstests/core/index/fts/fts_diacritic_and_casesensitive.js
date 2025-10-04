// Integration tests for {$diacriticSensitive: true, $caseSensitive: true} option to $text query
// operator.

import {queryIDS} from "jstests/libs/fts.js";

let coll = db.fts_diacritic_and_casesensitive;

coll.drop();

assert.commandWorked(
    coll.insert({_id: 0, a: "O próximo Vôo à Noite sobre o Atlântico, Põe Freqüentemente o único Médico."}),
);

assert.commandWorked(coll.createIndex({a: "text"}, {default_language: "portuguese"}));

assert.eq([0], queryIDS(coll, "próximo vôo à", null, {$diacriticSensitive: true, $caseSensitive: true}));
assert.eq([0], queryIDS(coll, "Atlântico", null, {$diacriticSensitive: true, $caseSensitive: true}));
assert.eq([0], queryIDS(coll, '"próximo"', null, {$diacriticSensitive: true, $caseSensitive: true}));
assert.eq([0], queryIDS(coll, '"Põe" Atlântico', null, {$diacriticSensitive: true, $caseSensitive: true}));
assert.eq([0], queryIDS(coll, '"próximo Vôo" "único Médico"', null, {$diacriticSensitive: true, $caseSensitive: true}));
assert.eq(
    [0],
    queryIDS(coll, '"próximo Vôo" -"único médico"', null, {$diacriticSensitive: true, $caseSensitive: true}),
);

assert.eq([], queryIDS(coll, "À", null, {$diacriticSensitive: true, $caseSensitive: true}));
assert.eq([], queryIDS(coll, "Próximo", null, {$diacriticSensitive: true, $caseSensitive: true}));
assert.eq([], queryIDS(coll, "proximo vôo à", null, {$diacriticSensitive: true, $caseSensitive: true}));
assert.eq([], queryIDS(coll, "À -próximo -Vôo", null, {$diacriticSensitive: true, $caseSensitive: true}));
assert.eq([], queryIDS(coll, "à proximo -Vôo", null, {$diacriticSensitive: true, $caseSensitive: true}));
assert.eq([], queryIDS(coll, "mo Vô", null, {$diacriticSensitive: true, $caseSensitive: true}));
assert.eq([], queryIDS(coll, '"único médico"', null, {$diacriticSensitive: true, $caseSensitive: true}));
assert.eq([], queryIDS(coll, '"próximo Vôo" -"único Médico"', null, {$diacriticSensitive: true, $caseSensitive: true}));
