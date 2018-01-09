// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoreplay

import (
	"testing"

	mgo "github.com/10gen/llmgo"
	"github.com/10gen/llmgo/bson"
)

// TestCommandsAgainstAuthedDBWhenAuthed tests some basic commands against a
// database that requires authentication when the driver has proper
// authentication credentials
func TestCommandsAgainstAuthedDBWhenAuthed(t *testing.T) {
	if !authTestServerMode {
		t.Skipf("Skipping auth test with non-auth DB")
	}
	if err := teardownDB(); err != nil {
		t.Error(err)
	}
	numInserts := 20
	generator := newRecordedOpGenerator()
	docName := "Authed Insert Test"

	go func() {
		defer close(generator.opChan)
		t.Logf("Generating %d inserts\n", numInserts)
		err := generator.generateInsertHelper(docName, 0, numInserts)
		if err != nil {
			t.Error(err)
		}
	}()
	statCollector, _ := newStatCollector(testCollectorOpts, "format", true, true)
	replaySession, err := mgo.Dial(urlAuth)
	if err != nil {
		t.Error(err)
	}

	context := NewExecutionContext(statCollector, replaySession, &ExecutionOptions{})
	t.Logf("Beginning mongoreplay playback of generated traffic against host: %v\n", urlAuth)
	err = Play(context, generator.opChan, testSpeed, 1, 10)
	if err != nil {
		t.Error(err)
	}
	t.Log("Completed mongoreplay playback of generated traffic")

	session, err := mgo.Dial(urlAuth)
	if err != nil {
		t.Error(err)
	}
	coll := session.DB(testDB).C(testCollection)

	iter := coll.Find(bson.D{}).Sort("docNum").Iter()
	ind := 0
	result := testDoc{}

	t.Log("Querying database to ensure insert occurred successfully")
	for iter.Next(&result) {
		t.Logf("Query result: %#v\n", result)
		if err := iter.Err(); err != nil {
			t.Errorf("Iterator returned an error: %v\n", err)
		}
		if result.DocumentNumber != ind {
			t.Errorf("Inserted document number did not match expected document number. Found: %v -- Expected: %v", result.DocumentNumber, ind)
		}
		if result.Name != "Authed Insert Test" {
			t.Errorf("Inserted document name did not match expected name. Found %v -- Expected: %v", result.Name, docName)
		}
		if !result.Success {
			t.Errorf("Inserted document field 'Success' was expected to be true, but was false")
		}
		ind++
	}
	if err := iter.Close(); err != nil {
		t.Error(err)
	}
	if err := teardownDB(); err != nil {
		t.Error(err)
	}

}

// TestCommandsAgainstAuthedDBWhenNotAuthed tests some basic commands against a
// database that requires authentication when the driver does not have proper
// authentication. It generates a series of inserts and ensures that the docs
// they are attempting to insert are not later found in the database
func TestCommandsAgainstAuthedDBWhenNotAuthed(t *testing.T) {
	if !authTestServerMode {
		t.Skipf("Skipping auth test with non-auth DB")
	}
	if err := teardownDB(); err != nil {
		t.Error(err)
	}
	numInserts := 3
	generator := newRecordedOpGenerator()

	go func() {
		defer close(generator.opChan)

		t.Logf("Generating %d inserts\n", numInserts)
		err := generator.generateInsertHelper("Non-Authed Insert Test", 0, numInserts)
		if err != nil {
			t.Error(err)
		}
	}()
	statCollector, _ := newStatCollector(testCollectorOpts, "format", true, true)
	replaySession, err := mgo.Dial(urlNonAuth)
	if err != nil {
		t.Error(err)
	}
	context := NewExecutionContext(statCollector, replaySession, &ExecutionOptions{})
	err = Play(context, generator.opChan, testSpeed, 1, 10)
	if err != nil {
		t.Error(err)
	}
	t.Log("Completed mongoreplay playback of generated traffic")

	session, err := mgo.Dial(urlAuth)
	if err != nil {
		t.Errorf("Error connecting to test server: %v", err)
	}
	coll := session.DB(testDB).C(testCollection)

	t.Log("Performing query to ensure collection received no documents")

	num, err := coll.Find(bson.D{}).Count()
	if err != nil {
		t.Error(err)
	}
	if num != 0 {
		t.Errorf("Collection contained documents, expected it to be empty. Num: %d\n", num)
	}
	if err := teardownDB(); err != nil {
		t.Error(err)
	}

}
