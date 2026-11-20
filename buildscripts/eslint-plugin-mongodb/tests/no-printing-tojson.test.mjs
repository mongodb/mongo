import assert from "node:assert/strict";
import test from "node:test";

import {Linter} from "eslint";

import rule from "../rules/no-printing-tojson.js";

const config = [
    {
        plugins: {
            mongodb: {
                rules: {
                    "no-printing-tojson": rule,
                },
            },
        },
        rules: {
            "mongodb/no-printing-tojson": "error",
        },
        languageOptions: {
            ecmaVersion: 2022,
        },
    },
];

function verify(code) {
    return new Linter().verify(code, config, "test.js");
}

function verifyAndFix(code) {
    return new Linter().verifyAndFix(code, config, "test.js");
}

test("flags direct jsTest.log.info calls with tojson arguments", () => {
    const messages = verify("jsTest.log.info(tojson(doc));");

    assert.equal(messages.length, 1);
    assert.match(messages[0].message, /jsTest\.log\.info\(\)/);
});

test("flags bracket-notation log helpers with tojson arguments", () => {
    const messages = verify('jsTest["log"].info(tojson(doc));');

    assert.equal(messages.length, 1);
    assert.match(messages[0].message, /jsTest\.log\.info\(\)/);
});

test("autofixes bracket-notation log helpers to use toJsonForLog", () => {
    const result = verifyAndFix('jsTest.log["info"](tojson(doc));');

    assert.equal(result.fixed, true);
    assert.equal(result.output, 'jsTest.log["info"](toJsonForLog(doc));');
});
