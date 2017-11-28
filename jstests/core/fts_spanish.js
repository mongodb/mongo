(function() {
    "use strict";

    load("jstests/libs/fts.js");

    const coll = db.text_spanish;
    coll.drop();

    assert.writeOK(coll.insert({_id: 1, title: "mi blog", text: "Este es un blog de prueba"}));
    assert.writeOK(
        coll.insert({_id: 2, title: "mi segundo post", text: "Este es un blog de prueba"}));
    assert.writeOK(coll.insert(
        {_id: 3, title: "cuchillos son divertidos", text: "este es mi tercer blog stemmed"}));
    assert.writeOK(coll.insert({
        _id: 4,
        language: "en",
        title: "My fourth blog",
        text: "This stemmed blog is in english"
    }));

    // Create a text index, giving more weight to the "title" field.
    assert.commandWorked(coll.createIndex({title: "text", text: "text"},
                                          {weights: {title: 10}, default_language: "es"}));

    assert.eq(4, coll.count({$text: {$search: "blog"}}));
    assert.eq([4], queryIDS(coll, "stem"));
    assert.eq([3], queryIDS(coll, "stemmed"));
    assert.eq([4], queryIDS(coll, "stemmed", null, {"$language": "en"}));
    assert.eq([1, 2], queryIDS(coll, "prueba").sort());

    assert.writeError(coll.insert({_id: 5, language: "spanglish", title: "", text: ""}));

    assert.commandWorked(coll.dropIndexes());
    assert.commandFailedWithCode(
        coll.createIndex({title: "text", text: "text"}, {default_language: "spanglish"}),
        ErrorCodes.CannotCreateIndex);
}());
