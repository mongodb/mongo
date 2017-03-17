//Test whether unknown commands are accepted in explain function

(function() {
	
	"use strict";
	
	var unknownCollection = db["jstestDB"];

	//insure the collection has an item
	unknownCollection.insert({a:1});

	//check with finding item that exist
	assert.commandFailed(db.runCommand({explain: {find: 'a'}, verbosity: 'queryPlanner', unknown: 1}), "unknown argument command extra");
	assert.commandFailed(db.runCommand({explain: {find: 'a'}, unknownArg: 'queryPlanner'}), "unknown verbosity command");
	assert.commandFailed(db.runCommand({explain: {find: 'a'}, unknownArg: 'excutionStats'}), "unknown verbosity command");
	assert.commandFailed(db.runCommand({explain: {find: 'a'}, unknownArg: 'allPlansExecution'}), "unknown verbosity command");
	assert.commandFailed(db.runCommand({explain: {find: 'a'}, unknownArg: 'unknown'}), "unknown argument and command");

	//empty collection
	unknownCollection.drop();

})();