// This test is meant to reproduce SERVER-38840, where the ICU library crashes on Windows when
// attempting to parse an invalid ID-prefixed locale.
(function() {
"use strict";

const coll = db.invalid_collation_locale;
coll.drop();

// Locale's which start with "x" or "i" followed by a separator ("_" or "-") are considered
// ID-prefixed.
assert.commandFailedWithCode(
    db.createCollection(coll.getName(), {collation: {locale: "x_invalid"}}), ErrorCodes.BadValue);

assert.commandFailedWithCode(
    db.createCollection(coll.getName(), {collation: {locale: "X_invalid"}}), ErrorCodes.BadValue);

assert.commandFailedWithCode(
    db.createCollection(coll.getName(), {collation: {locale: "i-invalid"}}), ErrorCodes.BadValue);

assert.commandFailedWithCode(
    db.createCollection(coll.getName(), {collation: {locale: "I-invalid"}}), ErrorCodes.BadValue);

assert.commandFailedWithCode(
    db.createCollection(coll.getName(), {collation: {locale: "xx_invalid"}}), ErrorCodes.BadValue);
})();
