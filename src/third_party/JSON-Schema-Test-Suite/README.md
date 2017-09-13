JSON Schema Test Suite [![Build Status](https://travis-ci.org/json-schema-org/JSON-Schema-Test-Suite.svg?branch=master)](https://travis-ci.org/json-schema-org/JSON-Schema-Test-Suite)
======================

This repository contains a set of JSON objects that implementors of JSON Schema
validation libraries can use to test their validators.

It is meant to be language agnostic and should require only a JSON parser.

The conversion of the JSON objects into tests within your test framework of
choice is still the job of the validator implementor.

Structure of a Test
-------------------

If you're going to use this suite, you need to know how tests are laid out. The
tests are contained in the `tests` directory at the root of this repository.

Inside that directory is a subdirectory for each draft or version of the
schema. We'll use `draft3` as an example.

If you look inside the draft directory, there are a number of `.json` files,
which logically group a set of test cases together. Often the grouping is by
property under test, but not always, especially within optional test files
(discussed below).

Inside each `.json` file is a single array containing objects. It's easiest to
illustrate the structure of these with an example:

```json
    {
        "description": "the description of the test case",
        "schema": {"the schema that should" : "be validated against"},
        "tests": [
            {
                "description": "a specific test of a valid instance",
                "data": "the instance",
                "valid": true
            },
            {
                "description": "another specific test this time, invalid",
                "data": 15,
                "valid": false
            }
        ]
    }
```

So a description, a schema, and some tests, where tests is an array containing
one or more objects with descriptions, data, and a boolean indicating whether
they should be valid or invalid.

Coverage
--------

Draft 3 and 4 should have full coverage. If you see anything missing or think
there is a useful test missing, please send a pull request or open an issue.

Who Uses the Test Suite
-----------------------

This suite is being used by:

### Coffeescript ###

* [jsck](https://github.com/pandastrike/jsck)

### C++ ###

* [Modern C++ JSON schema validator](https://github.com/pboettch/json-schema-validator)

### Dart ###

* [json_schema](https://github.com/patefacio/json_schema) 

### Elixir ###

* [ex_json_schema](https://github.com/jonasschmidt/ex_json_schema)

### Erlang ###

* [jesse](https://github.com/for-GET/jesse)

### Go ###

* [gojsonschema](https://github.com/sigu-399/gojsonschema) 
* [validate-json](https://github.com/cesanta/validate-json)

### Haskell ###

* [aeson-schema](https://github.com/timjb/aeson-schema)
* [hjsonschema](https://github.com/seagreen/hjsonschema)

### Java ###

* [json-schema-validator](https://github.com/daveclayton/json-schema-validator)
* [everit-org/json-schema](https://github.com/everit-org/json-schema)
* [networknt/json-schema-validator](https://github.com/networknt/json-schema-validator)

### JavaScript ###

* [json-schema-benchmark](https://github.com/Muscula/json-schema-benchmark)
* [direct-schema](https://github.com/IreneKnapp/direct-schema)
* [is-my-json-valid](https://github.com/mafintosh/is-my-json-valid)
* [jassi](https://github.com/iclanzan/jassi)
* [JaySchema](https://github.com/natesilva/jayschema)
* [json-schema-valid](https://github.com/ericgj/json-schema-valid)
* [Jsonary](https://github.com/jsonary-js/jsonary)
* [jsonschema](https://github.com/tdegrunt/jsonschema)
* [request-validator](https://github.com/bugventure/request-validator)
* [skeemas](https://github.com/Prestaul/skeemas)
* [tv4](https://github.com/geraintluff/tv4)
* [z-schema](https://github.com/zaggino/z-schema)
* [jsen](https://github.com/bugventure/jsen)
* [ajv](https://github.com/epoberezkin/ajv)
* [djv](https://github.com/korzio/djv)

### Node.js ###

The JSON Schema Test Suite is also available as an
[npm](https://www.npmjs.com/package/json-schema-test-suite) package.
Node-specific support is maintained on the [node branch](https://github.com/json-schema-org/JSON-Schema-Test-Suite/tree/node).
See [NODE-README.md](https://github.com/json-schema-org/JSON-Schema-Test-Suite/blob/node/NODE-README.md)
for more information.

### .NET ###

* [Newtonsoft.Json.Schema](https://github.com/JamesNK/Newtonsoft.Json.Schema)
* [Manatee.Json](https://github.com/gregsdennis/Manatee.Json)

### PHP ###

* [json-schema](https://github.com/justinrainbow/json-schema)
* [json-guard](https://github.com/thephpleague/json-guard)

### Python ###

* [jsonschema](https://github.com/Julian/jsonschema)

### Ruby ###

* [json-schema](https://github.com/hoxworth/json-schema)

### Rust ###

* [valico](https://github.com/rustless/valico)

### Swift ###

* [JSONSchema](https://github.com/kylef/JSONSchema.swift)

### Clojure ###

* [json-schema](https://github.com/tatut/json-schema)

### PostgreSQL ###

* [postgres-json-schema](https://github.com/gavinwahl/postgres-json-schema)

If you use it as well, please fork and send a pull request adding yourself to
the list :).

Contributing
------------

If you see something missing or incorrect, a pull request is most welcome!

There are some sanity checks in place for testing the test suite. You can run
them with `bin/jsonschema_suite check && npm test` or `tox && npm test`. They will be run automatically by
[Travis CI](https://travis-ci.org/) as well.
