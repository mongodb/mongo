"use strict";

const SharedSkunkState = (function() {
    // String Catalog
    const kStringCatalog = [
        "foo",
        "bar",
        "baz",
        "qux",
        "quux",
        "corge",
        "grault",
        "garply",
        "waldo",
        "fred",
        "plugh",
        "xyzzy",
        "thud"
    ];

    // Maximum value for the integer fields, minimum is 0
    const kMaxInt = 20;

    const kTemplateDoc = {
        field1: "",
        field2: "",
        field3: "",
        field4: 1,
        attributes: {
            attr1: "",
            attr2: "",
            attr3: 1,
            attr4: 1,
            attr5: 1,
            attr6: 1,
            attr7: 1,
            attr8: "",
            attr9: "",
            attr10: ""
        }
    };

    const kAttributeTemplateDoc = {
        field1: "",
        field2: "",
        field3: "",
        field4: 1,
        attributes: [
            {k: "attr1", v: ""},
            {k: "attr2", v: ""},
            {k: "attr3", v: 1},
            {k: "attr4", v: 1},
            {k: "attr5", v: 1},
            {k: "attr6", v: 1},
            {k: "attr7", v: 1},
            {k: "attr8", v: ""},
            {k: "attr9", v: ""},
            {k: "attr10", v: ""},
        ]
    };

    const kEqualityQueryTemplate = {
        "field1": "",
        "field2": "",
        "field3": "",
        "attributes": {
            "attr1": "",
            "attr2": "",
            "attr3": 1,
            "attr4": 1,
            "attr5": 1,
            "attr6": 1,
            "attr7": 1,
            "attr8": "",
            "attr9": "",
            "attr10": ""
        }
    };

    // name of the attrbiute field
    const kAttributesField = "attributes";

    const kAttributesToQuery = 10;

    /**
     * Generates a random dictated by the type of 'exampleValue'.
     */
    function getRandomValue(exampleValue) {
        switch (typeof exampleValue) {
            case "number":
                return Random.randInt(kMaxInt);

            case "boolean":
                return Random.randInt() % 2 == 0;

            case "object":
                if (exampleValue == null) {
                    return null
                }
                if (exampleValue instanceof Date) {
                    return new Date(kBaseDate.getTime() + (Random.rand() * kMaximumSeconds));
                }
                throw Error("Unknown type");

            case "string":
                return kStringCatalog[Random.randInt(kStringCatalog.length)];
        }
        throw Error("Unknown type");
    }

    // Randomness generator
    Random.setRandomSeed();
    return {
        getRandomValue: getRandomValue,
        kTemplateDoc: kTemplateDoc,
        kAttributeTemplateDoc: kAttributeTemplateDoc,
        kAttributesField: kAttributesField,
    };
})();

/*
let query = {
    "$and": [
        {"field1": "qux"},
        {"field2": "waldo"},
        {"field3": "baz"},
        {"attributes": {"$elemMatch": {"k": "attr4", "v": 5}}},
        {"attributes": {"$elemMatch": {"k": "attr7", "v": 13}}}
    ]
};
*/
