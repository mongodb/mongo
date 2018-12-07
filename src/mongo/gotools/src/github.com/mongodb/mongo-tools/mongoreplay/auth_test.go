package mongoreplay

import (
	"testing"

	mgo "github.com/10gen/llmgo"
	"github.com/10gen/llmgo/bson"
)

// TestCommandsAgainstAuthedDBWhenAuthed tests some basic commands against a
// database that requires authenticaiton when the driver has proper
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
	context := NewExecutionContext(statCollector)
	t.Logf("Beginning mongoreplay playback of generated traffic against host: %v\n", urlAuth)
	err := Play(context, generator.opChan, testSpeed, urlAuth, 1, 10)
	if err != nil {
		t.Error(err)
	}
	t.Log("Completed mongoreplay playback of generated traffic")

	session, err := mgo.Dial(urlAuth)
	coll := session.DB(testDB).C(testCollection)

	iter := coll.Find(bson.D{}).Sort("docNum").Iter()
	ind := 0
	result := testDoc{}

	t.Log("Querying database to ensure insert occured successfully")
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
// authenticaiton. It generates a series of inserts and ensures that the docs
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
	context := NewExecutionContext(statCollector)
	err := Play(context, generator.opChan, testSpeed, urlNonAuth, 1, 10)
	if err != nil {
		t.Error(err)
	}
	t.Log("Completed mongoreplay playback of generated traffic")

	session, err := mgo.Dial(urlAuth)
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
