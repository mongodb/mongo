/**
 * Validate auto complete works for various javascript types implemented by C++.
 */
(function() {
    'use strict';

    function testAutoComplete(prefix) {
        // This method updates a global object with an array of strings on success.
        shellAutocomplete(prefix);
        return __autocomplete__;
    }

    // Create a collection.
    db.auto_complete_coll.insert({});

    // Validate DB auto completion.
    const db_stuff = testAutoComplete('db.');

    // Verify we enumerate built-in methods.
    assert.contains('db.prototype', db_stuff);
    assert.contains('db.hasOwnProperty', db_stuff);
    assert.contains('db.toString(', db_stuff);

    // Verify we have some methods we added.
    assert.contains('db.adminCommand(', db_stuff);
    assert.contains('db.runCommand(', db_stuff);
    assert.contains('db.watch(', db_stuff);

    // Verify we enumerate collections.
    assert.contains('db.auto_complete_coll', db_stuff);

    // Validate Collection autocompletion.
    const coll_stuff = testAutoComplete('db.auto_complete_coll.');

    // Verify we enumerate built-in methods.
    assert.contains('db.auto_complete_coll.prototype', coll_stuff);
    assert.contains('db.auto_complete_coll.hasOwnProperty', coll_stuff);
    assert.contains('db.auto_complete_coll.toString(', coll_stuff);

    // Verify we have some methods we added.
    assert.contains('db.auto_complete_coll.aggregate(', coll_stuff);
    assert.contains('db.auto_complete_coll.runCommand(', coll_stuff);

    // Validate autocompletion when prefix is specified.
    const empty_stuff = testAutoComplete('');

    assert.contains('Array(', empty_stuff);
    assert.contains('print(', empty_stuff);
    assert.contains('ErrorCodes', empty_stuff);

    // Validate autocompletion returns ErrorCodes when ErrorCodes is specified.
    const error_codes_autocomplete = testAutoComplete('ErrorCodes.');

    assert.contains('ErrorCodes.BadValue', error_codes_autocomplete);
})();
