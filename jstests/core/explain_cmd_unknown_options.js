//Test whether unknown commands are accepted in explain function
(function() {
	
	"use strict";

	let collection = db.explain_cmd_unknown_options;
	collection.drop();

	//Ensure the collection is not empty
	collection.insert({a:1});

	//Test that the explain command rejects unknown options when the collection exists.
	assert.commandFailedWithCode(db.runCommand({explain: {find: collection.getName()}, verbosity: 'queryPlanner', unknownArg: 1}), ErrorCodes.FailedToParse);
	assert.commandFailedWithCode(db.runCommand({explain: {find: collection.getName()}, unknownArg: 'queryPlanner'}), ErrorCodes.FailedToParse);
	assert.commandFailedWithCode(db.runCommand({explain: {find: collection.getName()}, unknownArg: 'excutionStats'}), ErrorCodes.FailedToParse);
	assert.commandFailedWithCode(db.runCommand({explain: {find: collection.getName()}, unknownArg: 'allPlansExecution'}), ErrorCodes.FailedToParse);
	assert.commandFailedWithCode(db.runCommand({explain: {find: collection.getName()}, unknownArg: 'unknownArg'}), ErrorCodes.FailedToParse);
})();