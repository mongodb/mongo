// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoreplay

import (
	"fmt"
	"os"
	"strings"
	"testing"
	"time"

	mgo "github.com/10gen/llmgo"
	"github.com/10gen/llmgo/bson"
)

const (
	defaultTestPort      = "20000"
	nonAuthTestServerURL = "mongodb://localhost"
	authTestServerURL    = "mongodb://authorizedUser:authorizedPwd@localhost"
	testDB               = "mongoreplay"
	testCollection       = "test"
	testCursorID         = int64(12345)
	testSpeed            = float64(100)
)

var (
	testTime = time.Now()

	urlAuth, urlNonAuth string
	currentTestURL      string
	authTestServerMode  bool
	isMongosTestServer  bool
	testCollectorOpts   = StatOptions{
		Buffered: true,
	}
)

func setConnectionURL() error {
	testPort := os.Getenv("DB_PORT")
	if testPort == "" {
		testPort = defaultTestPort
	}
	var url string
	if os.Getenv("AUTH") == "1" {
		url = authTestServerURL
	} else {
		url = nonAuthTestServerURL
	}
	dialURL := fmt.Sprintf("%s:%v", url, testPort)
	if os.Getenv("AUTH") == "1" {
		dialURL += "/admin"
	}
	session, err := mgo.DialWithTimeout(dialURL, 30*time.Second)
	if err != nil {
		return fmt.Errorf("%v:%v", dialURL, err)
	}

	port, err := getPrimaryPort(session)
	if err != nil {
		return fmt.Errorf("%v:%v", dialURL, err)
	}
	if port == "" {
		port = testPort
	}
	urlNonAuth = fmt.Sprintf("%s:%s", nonAuthTestServerURL, port)
	urlAuth = fmt.Sprintf("%s:%v/admin", authTestServerURL, port)
	currentTestURL = urlNonAuth
	if os.Getenv("AUTH") == "1" {
		currentTestURL = urlAuth
	}
	return nil
}

func getPrimaryPort(session *mgo.Session) (string, error) {

	result := struct {
		Members []struct {
			Name     string `bson:"name"`
			StateStr string `bson:"stateStr"`
		} `bson:"members"`
	}{}

	res := &struct {
		Msg string
	}{}
	session.Run("ismaster", res)
	isMongosTestServer = (res.Msg == "isdbgrid")
	if isMongosTestServer {
		return "", nil
	}

	err := session.DB("admin").Run("replSetGetStatus", &result)

	if err != nil {
		if err.Error() == "not running with --replSet" {
			return "", nil
		}
		return "", err
	}

	for _, member := range result.Members {
		if member.StateStr == "PRIMARY" {
			return strings.Split(member.Name, ":")[1], nil
		}
	}

	return "", fmt.Errorf("replset status has no primary")
}

func TestMain(m *testing.M) {
	err := setConnectionURL()
	if err != nil {
		panic(err)
	}
	if os.Getenv("AUTH") == "1" {
		authTestServerMode = true
	} else {
		authTestServerMode = false
	}

	os.Exit(m.Run())
}

// TestOpInsertLiveDB tests the functionality of mongoreplay replaying inserts
// against a live database Generates 20 recorded inserts and passes them to the
// main execution of mongoreplay and queries the database to verify they were
// completed. It then checks its BufferedStatCollector to ensure the inserts
// match what we expected
func TestOpInsertLiveDB(t *testing.T) {
	if err := teardownDB(); err != nil {
		t.Error(err)
	}

	numInserts := 20
	insertName := "LiveDB Insert test"
	generator := newRecordedOpGenerator()
	go func() {
		defer close(generator.opChan)
		// generate numInserts RecordedOps
		t.Logf("Generating %d inserts\n", numInserts)
		err := generator.generateInsertHelper(insertName, 0, numInserts)
		if err != nil {
			t.Error(err)
		}
		t.Log("Generating getLastError")
		err = generator.generateGetLastError()
		if err != nil {
			t.Error(err)
		}
	}()

	statCollector, _ := newStatCollector(testCollectorOpts, "format", true, true)
	statRec := statCollector.StatRecorder.(*BufferedStatRecorder)
	replaySession, err := mgo.Dial(currentTestURL)
	if err != nil {
		t.Errorf("Error connecting to test server: %v", err)
	}
	context := NewExecutionContext(statCollector, replaySession, &ExecutionOptions{})

	// run mongoreplay's Play loop with the stubbed objects
	t.Logf("Beginning mongoreplay playback of generated traffic against host: %v\n", currentTestURL)
	err = Play(context, generator.opChan, testSpeed, 1, 10)
	if err != nil {
		t.Errorf("Error Playing traffic: %v\n", err)
	}
	t.Log("Completed mongoreplay playback of generated traffic")

	// prepare a query for the database
	session, err := mgo.DialWithTimeout(currentTestURL, 30*time.Second)
	if err != nil {
		t.Errorf("Error connecting to test server: %v", err)
	}

	coll := session.DB(testDB).C(testCollection)

	iter := coll.Find(bson.D{}).Sort("docNum").Iter()
	ind := 0
	result := testDoc{}

	// iterate over the results of the query and ensure they match expected documents
	t.Log("Querying database to ensure insert occured successfully")
	for iter.Next(&result) {
		t.Logf("Query result: %#v\n", result)
		if result.DocumentNumber != ind {
			t.Errorf("Inserted document number did not match expected document number. Found: %v -- Expected: %v", result.DocumentNumber, ind)
		}
		if result.Name != insertName {
			t.Errorf("Inserted document name did not match expected name. Found %v -- Expected: %v", result.Name, "LiveDB Insert Test")
		}
		if !result.Success {
			t.Errorf("Inserted document field 'Success' was expected to be true, but was false")
		}
		ind++
	}
	if err := iter.Close(); err != nil {
		t.Error(err)
	}

	// iterate over the operations found by the BufferedStatCollector
	t.Log("Examining collected stats to ensure they match expected")
	for i := 0; i < numInserts; i++ {
		stat := statRec.Buffer[i]
		t.Logf("Stat result: %#v\n", stat)
		// All commands should be inserts into mongoreplay.test
		if stat.OpType != "insert" ||
			stat.Ns != "mongoreplay.test" {
			t.Errorf("Expected to see an insert into mongoreplay.test, but instead saw %v, %v\n", stat.OpType, stat.Command)
		}
	}
	stat := statRec.Buffer[numInserts]
	t.Logf("Stat result: %#v\n", stat)
	if stat.OpType != "command" ||
		stat.Ns != "admin.$cmd" ||
		stat.Command != "getLastError" {
		t.Errorf("Epected to see a get last error, but instead saw %v, %v\n", stat.OpType, stat.Command)
	}
	if err := teardownDB(); err != nil {
		t.Error(err)
	}
}

// TestUpdateOpLiveDB tests the functionality of mongoreplay replaying an update
// against a live database Generates 20 recorded inserts and an update and
// passes them to the main execution of mongoreplay and queries the database to
// verify they were completed. It then checks its BufferedStatCollector to
// ensure the update matches what we expected.
func TestUpdateOpLiveDB(t *testing.T) {
	if err := teardownDB(); err != nil {
		t.Error(err)
	}

	numInserts := 20
	insertName := "LiveDB update test"
	generator := newRecordedOpGenerator()
	nameSpace := fmt.Sprintf("%s.%s", testDB, testCollection)
	flags := uint32(1<<1 | 1)

	docNum := bson.D{{"$lte", 9.0}}
	selector := bson.D{{"docNum", docNum}}
	change := bson.D{{"updated", true}}
	update := bson.D{{"$set", change}}
	updateOp := mgo.UpdateOp{
		Collection: nameSpace,
		Selector:   selector,
		Update:     update,
		Flags:      flags,
		Multi:      true,
		Upsert:     true,
	}

	go func() {
		defer close(generator.opChan)
		// generate numInserts RecordedOps
		t.Logf("Generating %d inserts\n", numInserts)
		err := generator.generateInsertHelper(insertName, 0, numInserts)
		if err != nil {
			t.Error(err)
		}
		recordedUpdate, err := generator.fetchRecordedOpsFromConn(&updateOp)
		if err != nil {
			t.Error(err)
		}
		generator.pushDriverRequestOps(recordedUpdate)

		t.Log("Generating getLastError")
		err = generator.generateGetLastError()
		if err != nil {
			t.Error(err)
		}
	}()

	statCollector, _ := newStatCollector(testCollectorOpts, "format", true, true)
	statRec := statCollector.StatRecorder.(*BufferedStatRecorder)
	replaySession, err := mgo.Dial(currentTestURL)
	if err != nil {
		t.Errorf("Error connecting to test server: %v", err)
	}
	context := NewExecutionContext(statCollector, replaySession, &ExecutionOptions{})

	// run mongoreplay's Play loop with the stubbed objects
	t.Logf("Beginning mongoreplay playback of generated traffic against host: %v\n", currentTestURL)
	err = Play(context, generator.opChan, testSpeed, 1, 10)
	if err != nil {
		t.Errorf("Error Playing traffic: %v\n", err)
	}
	t.Log("Completed mongoreplay playback of generated traffic")

	// prepare a query for the database
	session, err := mgo.DialWithTimeout(currentTestURL, 30*time.Second)
	if err != nil {
		t.Errorf("Error connecting to test server: %v", err)
	}

	coll := session.DB(testDB).C(testCollection)

	iter := coll.Find(bson.D{}).Sort("docNum").Iter()
	ind := 0
	result := struct {
		DocumentNumber int    `bson:"docNum"`
		Name           string `bson:"name"`
		Updated        bool   `bson:"updated"`
	}{}

	// iterate over the results of the query and ensure they match expected documents
	t.Log("Querying database to ensure insert occured successfully")
	for iter.Next(&result) {
		t.Logf("Query result: %#v\n", result)
		if result.DocumentNumber != ind {
			t.Errorf("Inserted document number did not match expected document number. Found: %v -- Expected: %v", result.DocumentNumber, ind)
		}
		if result.Name != insertName {
			t.Errorf("Inserted document name did not match expected name. Found %v -- Expected: %v", result.Name, "LiveDB update test")
		}
		if result.DocumentNumber <= 9 {
			if result.Updated != true {
				t.Errorf("Document with number %v was supposed to be updated but wasn't", result.DocumentNumber)
			}
		}
		ind++
	}
	if err := iter.Close(); err != nil {
		t.Error(err)
	}

	// iterate over the operations found by the BufferedStatCollector
	t.Log("Examining collected stats to ensure they match expected")

	stat := statRec.Buffer[numInserts]
	t.Logf("Stat result: %#v\n", stat)
	// All commands should be inserts into mongoreplay.test
	if stat.OpType != "update" ||
		stat.Ns != "mongoreplay.test" {
		t.Errorf("Expected to see an update to mongoreplay.test, but instead saw %v, %v\n", stat.OpType, stat.Ns)
	}
	if err := teardownDB(); err != nil {
		t.Error(err)
	}
}

// TestQueryOpLiveDB tests the functionality of some basic queries through mongoreplay.
// It generates inserts and queries and sends them to the main execution of mongoreplay.
// TestQueryOp then examines a BufferedStatCollector to ensure the queries executed as expected
func TestQueryOpLiveDB(t *testing.T) {
	if err := teardownDB(); err != nil {
		t.Error(err)
	}

	insertName := "LiveDB Query Test"
	numInserts := 20
	numQueries := 4
	generator := newRecordedOpGenerator()
	go func() {
		defer close(generator.opChan)
		// generate numInsert inserts and feed them to the opChan for tape
		for i := 0; i < numQueries; i++ {
			t.Logf("Generating %d inserts\n", numInserts/numQueries)
			err := generator.generateInsertHelper(fmt.Sprintf("%s: %d", insertName, i), i*(numInserts/numQueries), numInserts/numQueries)
			if err != nil {
				t.Error(err)
			}
		}
		// generate numQueries queries and feed them to the opChan for play
		t.Logf("Generating %d queries\n", numQueries+1)
		for j := 0; j < numQueries; j++ {
			querySelection := bson.D{{"name", fmt.Sprintf("LiveDB Query Test: %d", j)}}
			err := generator.generateQuery(querySelection, 0, 0)
			if err != nil {
				t.Error(err)
			}
		}
		// generate another query that tests a different field
		querySelection := bson.D{{"success", true}}
		err := generator.generateQuery(querySelection, 0, 0)
		if err != nil {
			t.Error(err)
		}
	}()

	statCollector, _ := newStatCollector(testCollectorOpts, "format", true, true)
	statRec := statCollector.StatRecorder.(*BufferedStatRecorder)
	replaySession, err := mgo.Dial(currentTestURL)
	if err != nil {
		t.Errorf("Error connecting to test server: %v", err)
	}
	context := NewExecutionContext(statCollector, replaySession, &ExecutionOptions{})

	// run mongoreplay's Play loop with the stubbed objects
	t.Logf("Beginning mongoreplay playback of generated traffic against host: %v\n", currentTestURL)
	err = Play(context, generator.opChan, testSpeed, 1, 10)
	if err != nil {
		t.Errorf("Error Playing traffic: %v\n", err)
	}
	t.Log("Completed mongoreplay playback of generated traffic")

	t.Log("Examining collected stats to ensure they match expected")
	// loop over the BufferedStatCollector for each of the numQueries queries
	// created to ensure that they match what we expected mongoreplay to have
	// executed
	for i := 0; i < numQueries; i++ {
		stat := statRec.Buffer[numInserts+i]
		t.Logf("Stat result: %#v\n", stat)
		if stat.OpType != "query" ||
			stat.Ns != "mongoreplay.test" ||
			stat.NumReturned != 5 {
			t.Errorf("Query Not Matched %#v\n", stat)
		}
	}
	stat := statRec.Buffer[numInserts+numQueries]
	t.Logf("Stat result: %#v\n", stat)
	// ensure that the last query that was making a query on the 'success' field
	// executed how we expected
	if stat.OpType != "query" ||
		stat.Ns != "mongoreplay.test" ||
		stat.NumReturned != 20 {
		t.Errorf("Query Not Matched %#v\n", stat)
	}
	if err := teardownDB(); err != nil {
		t.Error(err)
	}

}

// TestOpGetMoreLiveDB tests the functionality of a getmore command played
// through mongoreplay. It generates inserts, a query, and a series of getmores
// based on the original query. It then Uses a BufferedStatCollector to ensure
// the getmores executed as expected
func TestOpGetMoreLiveDB(t *testing.T) {
	if err := teardownDB(); err != nil {
		t.Error(err)
	}
	generator := newRecordedOpGenerator()
	insertName := "LiveDB Getmore Test"

	gmHelperNames := getmoreHelperNames{
		findOpType:    "query",
		getmoreOpType: "getmore",
		namespace:     "mongoreplay.test",
		insertName:    insertName,
	}

	getmoreTestHelper(t, gmHelperNames, generator.opChan, generator.generateInsertHelper,
		generator.generateGetMore, generator.generateReply, generator.generateQuery)

	if err := teardownDB(); err != nil {
		t.Error(err)
	}
}

// TestOpGetMoreMultiCursorLiveDB tests the functionality of getmores using
// multiple cursors against a live database. It generates a series of inserts
// followed by two seperate queries. It then uses each of those queries to
// generate multiple getmores. TestOpGetMoreMultiCursorLiveDB uses a
// BufferedStatCollector to ensure that each getmore played against the database
// is executed and recieves the response expected
func TestOpGetMoreMultiCursorLiveDB(t *testing.T) {
	if err := teardownDB(); err != nil {
		t.Error(err)
	}
	generator := newRecordedOpGenerator()
	var cursor1 int64 = 123
	var cursor2 int64 = 456
	numInserts := 20
	insertName := "LiveDB Multi-Cursor GetMore Test"
	numGetMoresLimit5 := 3
	numGetMoresLimit2 := 9
	go func() {
		defer close(generator.opChan)

		// generate numInserts RecordedOp inserts and send them to a channel to
		// be played in mongoreplay's main Play function
		t.Logf("Generating %v inserts\n", numInserts)
		err := generator.generateInsertHelper(insertName, 0, numInserts)
		if err != nil {
			t.Error(err)
		}
		querySelection := bson.D{{}}
		var responseID1 int32 = 2
		// generate a first query with a known requestID
		t.Log("Generating query/reply pair")
		err = generator.generateQuery(querySelection, 2, responseID1)
		if err != nil {
			t.Error(err)
		}

		// generate a reply with a known cursorID to be the direct response to the first Query
		err = generator.generateReply(responseID1, cursor1)
		if err != nil {
			t.Error(err)
		}

		var responseID2 int32 = 3
		t.Log("Generating query/reply pair")
		// generate a second query with a different requestID
		err = generator.generateQuery(querySelection, 5, responseID2)
		if err != nil {
			t.Error(err)
		}
		// generate a reply to the second query with another known cursorID
		err = generator.generateReply(responseID2, cursor2)
		if err != nil {
			t.Error(err)
		}
		t.Logf("Generating interleaved getmores")
		// Issue a series of interleaved getmores using the different cursorIDs
		for i := 0; i < numGetMoresLimit2; i++ {
			if i < numGetMoresLimit5 {
				// generate a series of getMores using cursorID2 and a limit of 5
				err = generator.generateGetMore(cursor2, 5)
				if err != nil {
					t.Error(err)
				}
			}
			// generate a series of getMores using cursorID2 and a limit of 5
			err = generator.generateGetMore(cursor1, 2)
			if err != nil {
				t.Error(err)
			}
		}
	}()
	statCollector, _ := newStatCollector(testCollectorOpts, "format", true, true)
	statRec := statCollector.StatRecorder.(*BufferedStatRecorder)
	replaySession, err := mgo.Dial(currentTestURL)
	if err != nil {
		t.Errorf("Error connecting to test server: %v", err)
	}
	context := NewExecutionContext(statCollector, replaySession, &ExecutionOptions{})

	// run mongoreplay's Play loop with the stubbed objects
	t.Logf("Beginning mongoreplay playback of generated traffic against host: %v\n", currentTestURL)
	err = Play(context, generator.opChan, testSpeed, 1, 10)
	if err != nil {
		t.Errorf("Error Playing traffic: %v\n", err)
	}
	t.Log("Completed mongoreplay playback of generated traffic")

	shouldBeLimit5 := true
	var limit int
	totalGetMores := numGetMoresLimit5 + numGetMoresLimit2

	t.Log("Examining collected getMores to ensure they match expected")
	// loop over the total number of getmores played at their expected positions
	// in the BufferedStatCollector, and ensure that the first set of getmores
	// should be alternating between having a limit of 5 and a limit of 2, and
	// after seeing all those with a limit of 5 we should see exclusively
	// getmores with limit 2
	for i := 0; i < totalGetMores; i++ {
		stat := statRec.Buffer[numInserts+2+i]
		t.Logf("Stat result: %#v\n", stat)
		if i < numGetMoresLimit5*2 && shouldBeLimit5 {
			limit = 5
			shouldBeLimit5 = !shouldBeLimit5
		} else {
			shouldBeLimit5 = !shouldBeLimit5
			limit = 2
		}
		if stat.OpType != "getmore" ||
			stat.NumReturned != limit ||
			stat.Ns != "mongoreplay.test" {
			t.Errorf("Getmore Not matched: %#v\n", stat)
		}
	}
	if err := teardownDB(); err != nil {
		t.Error(err)
	}
}

// TestOpKillCursorsLiveDB tests the functionality of killcursors using multiple
// cursors against a live database. It generates a series of inserts followed
// by two seperate queries. It then uses each of those queries to generate
// multiple getmores. Finally, it runs a killcursors op and one getmore for
// each killed cursor TestOpKillCursorsLiveDB uses a BufferedStatCollector to
// ensure that each killcursors played against the database is executed and
// recieves the response expected
func TestOpKillCursorsLiveDB(t *testing.T) {
	if err := teardownDB(); err != nil {
		t.Error(err)
	}
	generator := newRecordedOpGenerator()
	var cursor1 int64 = 123
	var cursor2 int64 = 456
	numInserts := 20
	insertName := "LiveDB KillCursors Test"
	go func() {
		defer close(generator.opChan)

		// generate numInserts RecordedOp inserts and send them to a channel to be played in mongoreplay's main Play function
		t.Logf("Generating %v inserts\n", numInserts)
		err := generator.generateInsertHelper(insertName, 0, numInserts)
		if err != nil {
			t.Error(err)
		}
		querySelection := bson.D{{}}
		var responseID1 int32 = 2
		// generate a first query with a known requestID
		t.Log("Generating query/reply pair")
		err = generator.generateQuery(querySelection, 2, responseID1)
		if err != nil {
			t.Error(err)
		}

		// generate a reply with a known cursorID to be the direct response to the first Query
		err = generator.generateReply(responseID1, cursor1)
		if err != nil {
			t.Error(err)
		}

		var responseID2 int32 = 3
		t.Log("Generating query/reply pair")
		// generate a second query with a different requestID
		err = generator.generateQuery(querySelection, 2, responseID2)
		if err != nil {
			t.Error(err)
		}
		// generate a reply to the second query with another known cursorID
		err = generator.generateReply(responseID2, cursor2)
		if err != nil {
			t.Error(err)
		}
		t.Logf("Generating getmores")
		// issue two getmores, one with each cursorID
		err = generator.generateGetMore(cursor2, 2)
		if err != nil {
			t.Error(err)
		}
		err = generator.generateGetMore(cursor1, 2)
		if err != nil {
			t.Error(err)
		}
		t.Logf("Generating killcursors")
		// issue a KillCursor to kill these cursors on the database
		err = generator.generateKillCursors([]int64{cursor1, cursor2})
		if err != nil {
			t.Error(err)
		}
		// issue two more getmores, one with each cursorID that should have been killed
		err = generator.generateGetMore(cursor2, 2)
		if err != nil {
			t.Error(err)
		}
		err = generator.generateGetMore(cursor1, 2)
		if err != nil {
			t.Error(err)
		}
	}()
	statCollector, _ := newStatCollector(testCollectorOpts, "format", true, true)
	statRec := statCollector.StatRecorder.(*BufferedStatRecorder)
	replaySession, err := mgo.Dial(currentTestURL)
	if err != nil {
		t.Errorf("Error connecting to test server: %v", err)
	}
	context := NewExecutionContext(statCollector, replaySession, &ExecutionOptions{})

	// run mongoreplay's Play loop with the stubbed objects
	t.Logf("Beginning mongoreplay playback of generated traffic against host: %v\n", currentTestURL)
	err = Play(context, generator.opChan, testSpeed, 1, 10)
	if err != nil {
		t.Errorf("Error Playing traffic: %v\n", err)
	}
	t.Log("Completed mongoreplay playback of generated traffic")

	t.Log("Examining collected getMores and replies to ensure they match expected")

	for i := 0; i < 2; i++ {
		stat := statRec.Buffer[numInserts+2+i]
		t.Logf("Stat result: %#v\n", stat)
		if stat.OpType != "getmore" ||
			stat.NumReturned != 2 ||
			stat.Ns != "mongoreplay.test" {
			t.Errorf("Getmore Not matched: Expected OpType: %s NumReturned: %d NameSpace: %s --- Found OpType: %s NumReturned: %d NameSpace: %s",
				"getmore", 2, "mongoreplay.test", stat.OpType, stat.NumReturned, stat.Ns)
		}
	}
	stat := statRec.Buffer[numInserts+2+2]
	t.Logf("Stat result: %#v\n", stat)
	if stat.OpType != "killcursors" {
		t.Errorf("Killcursors Not matched: Expected OpType: %s --- Found OpType: %s",
			"killcursors", stat.OpType)
	}
	for i := 0; i < 2; i++ {
		stat := statRec.Buffer[numInserts+5+i]
		t.Logf("Stat result: %#v\n", stat)
		if stat.OpType != "getmore" ||
			stat.NumReturned != 0 ||
			stat.Ns != "mongoreplay.test" {
			t.Errorf("Getmore Not matched: Expected OpType: %s NumReturned: %d NameSpace: %s --- Found OpType: %s NumReturned: %d NameSpace: %s",
				"getmore", 0, "mongoreplay.test", stat.OpType, stat.NumReturned, stat.Ns)
		}
	}

	if err := teardownDB(); err != nil {
		t.Error(err)
	}
}
func TestCommandOpInsertLiveDB(t *testing.T) {
	if err := teardownDB(); err != nil {
		t.Error(err)
	}
	if isMongosTestServer {
		t.Skipf("Skipping OpCommand test against mongos")
	}

	numInserts := 20
	insertName := "LiveDB CommandOp insert test"
	generator := newRecordedOpGenerator()
	go func() {
		defer close(generator.opChan)
		// generate numInserts RecordedOps
		t.Logf("Generating %d commandOp inserts\n", numInserts)
		err := generator.generateCommandOpInsertHelper(insertName, 0, numInserts)
		if err != nil {
			t.Error(err)
		}
	}()

	statCollector, _ := newStatCollector(testCollectorOpts, "format", true, true)
	statRec := statCollector.StatRecorder.(*BufferedStatRecorder)
	replaySession, err := mgo.Dial(currentTestURL)
	if err != nil {
		t.Errorf("Error connecting to test server: %v", err)
	}
	context := NewExecutionContext(statCollector, replaySession, &ExecutionOptions{})

	// run mongoreplay's Play loop with the stubbed objects
	t.Logf("Beginning mongoreplay playback of generated traffic against host: %v\n", currentTestURL)
	err = Play(context, generator.opChan, testSpeed, 1, 10)
	if err != nil {
		t.Errorf("Error Playing traffic: %v\n", err)
	}
	t.Log("Completed mongoreplay playback of generated traffic")

	// prepare a query for the database
	session, err := mgo.DialWithTimeout(currentTestURL, 30*time.Second)
	if err != nil {
		t.Errorf("Error connecting to test server: %v", err)
	}

	coll := session.DB(testDB).C(testCollection)

	iter := coll.Find(bson.D{}).Sort("docNum").Iter()
	ind := 0
	result := testDoc{}

	// iterate over the results of the query and ensure they match expected documents
	t.Log("Querying database to ensure insert occured successfully")
	for iter.Next(&result) {
		t.Logf("Query result: %#v\n", result)
		if result.DocumentNumber != ind {
			t.Errorf("Inserted document number did not match expected document number. Found: %v -- Expected: %v", result.DocumentNumber, ind)
		}
		if result.Name != insertName {
			t.Errorf("Inserted document name did not match expected name. Found %v -- Expected: %v", result.Name, "LiveDB Insert Test")
		}
		if !result.Success {
			t.Errorf("Inserted document field 'Success' was expected to be true, but was false")
		}
		ind++
	}
	if err := iter.Close(); err != nil {
		t.Error(err)
	}

	// iterate over the operations found by the BufferedStatCollector
	t.Log("Examining collected stats to ensure they match expected")
	for i := 0; i < numInserts; i++ {
		stat := statRec.Buffer[i]
		t.Logf("Stat result: %#v\n", stat)
		// all commands should be inserts into mongoreplay.test
		if stat.OpType != "op_command" ||
			stat.Command != "insert" ||
			stat.Ns != "mongoreplay" {
			t.Errorf("Expected to see an insert into mongoreplay, but instead saw %v, %v into %v\n", stat.OpType, stat.Command, stat.Ns)
		}
	}

	if err := teardownDB(); err != nil {
		t.Error(err)
	}
}

func TestCommandOpFindLiveDB(t *testing.T) {
	if err := teardownDB(); err != nil {
		t.Error(err)
	}
	if isMongosTestServer {
		t.Skipf("Skipping OpCommand test against mongos")
	}

	insertName := "LiveDB CommandOp Find Test"
	numInserts := 20
	numFinds := 4
	generator := newRecordedOpGenerator()
	go func() {
		defer close(generator.opChan)
		// generate numInsert inserts and feed them to the opChan for tape
		for i := 0; i < numFinds; i++ {
			t.Logf("Generating %d inserts\n", numInserts/numFinds)
			err := generator.generateInsertHelper(fmt.Sprintf("%s: %d", insertName, i), i*(numInserts/numFinds), numInserts/numFinds)
			if err != nil {
				t.Error(err)
			}
		}
		// generate numFinds queries and feed them to the opChan for play
		t.Logf("Generating %d finds\n", numFinds)
		for j := 0; j < numFinds; j++ {
			filter := bson.D{{"name", fmt.Sprintf("%s: %d", insertName, j)}}
			err := generator.generateCommandFind(filter, 0, 0)
			if err != nil {
				t.Error(err)
			}
		}
		// generate another query that tests a different field
		filter := bson.D{{"success", true}}
		err := generator.generateCommandFind(filter, 0, 0)
		if err != nil {
			t.Error(err)
		}
	}()

	statCollector, _ := newStatCollector(testCollectorOpts, "format", true, true)
	statRec := statCollector.StatRecorder.(*BufferedStatRecorder)
	replaySession, err := mgo.Dial(currentTestURL)
	if err != nil {
		t.Errorf("Error connecting to test server: %v", err)
	}
	context := NewExecutionContext(statCollector, replaySession, &ExecutionOptions{})

	// run mongoreplay's Play loop with the stubbed objects
	t.Logf("Beginning mongoreplay playback of generated traffic against host: %v\n", currentTestURL)
	err = Play(context, generator.opChan, testSpeed, 1, 10)
	if err != nil {
		t.Errorf("Error Playing traffic: %v\n", err)
	}
	t.Log("Completed mongoreplay playback of generated traffic")

	t.Log("Examining collected stats to ensure they match expected")
	opType := "op_command"
	ns := "mongoreplay"
	numReturned := 5
	// loop over the BufferedStatCollector for each of the numFinds queries
	// created, and ensure that they match what we expected mongoreplay to have
	// executed
	for i := 0; i < numFinds; i++ {
		stat := statRec.Buffer[numInserts+i]
		t.Logf("Stat result: %#v\n", stat)
		if stat.OpType != opType ||
			stat.Ns != ns ||
			stat.NumReturned != numReturned {
			t.Errorf("CommandOp Find Not Matched expected OpType: %s, Ns: %s, NumReturned: %d ---- found OpType: %s, Ns: %s, NumReturned: %d\n",
				opType, ns, numReturned, stat.OpType, stat.Ns, stat.NumReturned)
		}
	}
	stat := statRec.Buffer[numInserts+numFinds]
	t.Logf("Stat result: %#v\n", stat)
	// ensure that the last query that was making a query on the 'success' field
	// executed as expected
	if stat.OpType != opType ||
		stat.Ns != ns ||
		stat.NumReturned != 20 {
		t.Errorf("CommandOp Find Not Matched expected OpType: %s, Ns: %s, NumReturned: %d ---- found OpType: %s, Ns: %s, NumReturned: %d\n",
			opType, ns, 20, stat.OpType, stat.Ns, stat.NumReturned)
	}
	if err := teardownDB(); err != nil {
		t.Error(err)
	}

}

func TestCommandOpGetMoreLiveDB(t *testing.T) {
	if isMongosTestServer {
		t.Skipf("Skipping OpCommand test against mongos")
	}
	if err := teardownDB(); err != nil {
		t.Error(err)
	}
	generator := newRecordedOpGenerator()
	insertName := "LiveDB CommandGetmore Test"

	gmHelperNames := getmoreHelperNames{
		findOpType:         "op_command",
		findCommandName:    "find",
		getmoreOpType:      "op_command",
		getmoreCommandName: "getMore",
		namespace:          "mongoreplay",
		insertName:         insertName,
	}

	getmoreTestHelper(t, gmHelperNames, generator.opChan, generator.generateInsertHelper,
		generator.generateCommandGetMore, generator.generateCommandReply, generator.generateCommandFind)

	if err := teardownDB(); err != nil {
		t.Error(err)
	}
}

// TestMsgOpInsertLiveDB tests the functionality of mongoreplay replaying inserts
// against a live database Generates 20 recorded inserts and passes them to the
// main execution of mongoreplay and queries the database to verify they were
// completed. It then checks its BufferedStatCollector to ensure the inserts
// match what we expected
func TestMsgOpInsertLiveDB(t *testing.T) {
	if err := teardownDB(); err != nil {
		t.Error(err)
	}

	numInserts := 20
	insertName := "LiveDB Insert test"
	generator := newRecordedOpGenerator()
	go func() {
		defer close(generator.opChan)
		// generate numInserts RecordedOps
		t.Logf("Generating %d msg_op inserts\n", numInserts)
		err := generator.generateMsgOpInsertHelper(insertName, 0, numInserts)
		if err != nil {
			t.Error(err)
		}
	}()

	statCollector, _ := newStatCollector(testCollectorOpts, "format", true, true)
	statRec := statCollector.StatRecorder.(*BufferedStatRecorder)
	replaySession, err := mgo.Dial(currentTestURL)
	if err != nil {
		t.Errorf("Error connecting to test server: %v", err)
	}
	context := NewExecutionContext(statCollector, replaySession, &ExecutionOptions{})

	// run mongoreplay's Play loop with the stubbed objects
	t.Logf("Beginning mongoreplay playback of generated traffic against host: %v\n", currentTestURL)
	err = Play(context, generator.opChan, testSpeed, 1, 10)
	if err != nil {
		t.Errorf("Error Playing traffic: %v\n", err)
	}
	t.Log("Completed mongoreplay playback of generated traffic")

	// prepare a query for the database
	session, err := mgo.DialWithTimeout(currentTestURL, 30*time.Second)
	if err != nil {
		t.Errorf("Error connecting to test server: %v", err)
	}

	coll := session.DB(testDB).C(testCollection)

	iter := coll.Find(bson.D{}).Sort("docNum").Iter()
	ind := 0
	result := testDoc{}

	// iterate over the results of the query and ensure they match expected documents
	t.Log("Querying database to ensure insert occured successfully")
	for iter.Next(&result) {
		t.Logf("Query result: %#v\n", result)
		if result.DocumentNumber != ind {
			t.Errorf("Inserted document number did not match expected document number. Found: %v -- Expected: %v", result.DocumentNumber, ind)
		}
		if result.Name != insertName {
			t.Errorf("Inserted document name did not match expected name. Found %v -- Expected: %v", result.Name, "LiveDB Insert Test")
		}
		if !result.Success {
			t.Errorf("Inserted document field 'Success' was expected to be true, but was false")
		}
		ind++
	}
	if err := iter.Close(); err != nil {
		t.Error(err)
	}

	// iterate over the operations found by the BufferedStatCollector
	t.Log("Examining collected stats to ensure they match expected")
	for i := 0; i < numInserts; i++ {
		stat := statRec.Buffer[i]
		t.Logf("Stat result: %#v\n", stat)
		// All commands should be inserts into mongoreplay.test
		if stat.OpType != "op_msg" {
			t.Errorf("Expected to see an op with type op_msg, but instead saw %v", stat.OpType)
			continue
		}
	}
	if err := teardownDB(); err != nil {
		t.Error(err)
	}
}

func TestMsgOpGetMoreLiveDB(t *testing.T) {
	if err := teardownDB(); err != nil {
		t.Error(err)
	}
	generator := newRecordedOpGenerator()
	insertName := "LiveDB MsgGetmore Test"

	gmHelperNames := getmoreHelperNames{
		findOpType:         "op_msg",
		findCommandName:    "find",
		getmoreOpType:      "op_msg",
		getmoreCommandName: "getMore",
		namespace:          "mongoreplay",
		insertName:         insertName,
	}
	getmoreTestHelper(t, gmHelperNames, generator.opChan, generator.generateMsgOpInsertHelper,
		generator.generateMsgOpGetMore, generator.generateMsgOpReply, generator.generateMsgOpFind)

	if err := teardownDB(); err != nil {
		t.Error(err)
	}
}

type insertGeneratorFunc func(string, int, int) error
type replyGeneratorFunc func(int32, int64) error
type getmoreGeneratorFunc func(int64, int32) error
type findGeneratorFunc func(interface{}, int32, int32) error

type getmoreHelperNames struct {
	findOpType         string
	findCommandName    string
	getmoreOpType      string
	getmoreCommandName string
	namespace          string
	insertName         string
}

// getmoreTestHelper takes the functions that will produce the operations for test run
// and run them against the database.
func getmoreTestHelper(t *testing.T,
	gmHelperNames getmoreHelperNames,
	opChan chan *RecordedOp,
	insertFunc insertGeneratorFunc,
	getmoreFunc getmoreGeneratorFunc,
	replyFunc replyGeneratorFunc,
	findFunc findGeneratorFunc) {

	var requestID int32 = 2
	numInserts := 20
	numGetMores := 3
	go func() {
		defer close(opChan)
		t.Logf("Generating %d inserts\n", numInserts)
		// generate numInserts RecordedOp inserts and push them into a channel
		// for use in mongoreplay's Play()
		err := insertFunc(gmHelperNames.insertName, 0, numInserts)
		if err != nil {
			t.Error(err)
		}
		querySelection := bson.D{}

		t.Log("Generating find")
		// generate a query with a known requestID to be played in mongoreplay
		err = findFunc(querySelection, 5, requestID)
		if err != nil {
			t.Error(err)
		}
		t.Log("Generating reply")
		// generate a RecordedOp reply whose ResponseTo field matches that of
		// the original with a known cursorID so that these pieces of
		// information can be correlated by mongoreplay
		err = replyFunc(requestID, testCursorID)
		if err != nil {
			t.Error(err)
		}
		t.Log("Generating GetMore")
		// generate numGetMores RecordedOp getmores with a cursorID matching
		// that of the found reply
		for i := 0; i < numGetMores; i++ {
			err = getmoreFunc(testCursorID, 5)
			if err != nil {
				t.Error(err)
			}
		}
	}()
	statCollector, _ := newStatCollector(testCollectorOpts, "format", true, true)
	statRec := statCollector.StatRecorder.(*BufferedStatRecorder)
	replaySession, err := mgo.Dial(currentTestURL)
	if err != nil {
		t.Errorf("Error connecting to test server: %v", err)
	}
	context := NewExecutionContext(statCollector, replaySession, &ExecutionOptions{})

	// run mongoreplay's Play loop with the stubbed objects
	t.Logf("Beginning mongoreplay playback of generated traffic against host: %v\n", currentTestURL)
	err = Play(context, opChan, testSpeed, 1, 10)
	if err != nil {
		t.Errorf("Error Playing traffic: %v\n", err)
	}
	t.Log("Completed mongoreplay playback of generated traffic")

	t.Log("Examining collected stats to ensure they match expected")
	find := statRec.Buffer[numInserts]
	if find.OpType != gmHelperNames.findOpType ||
		find.Command != gmHelperNames.findCommandName ||
		find.NumReturned != 5 {
		t.Errorf("Find not matched. Expected OpType %s Command %s NumReturned %d --- Found OpType %s Command %s NumReturned: %d", gmHelperNames.findOpType, gmHelperNames.findCommandName, 5, find.OpType, find.Command, find.NumReturned)
	}
	// loop over the BufferedStatCollector in the positions the getmores should
	// have been played, and ensure that each getMore matches the criteria we
	// expected it to have
	for i := 0; i < numGetMores; i++ {
		getmoreStat := statRec.Buffer[numInserts+1+i]
		t.Logf("Stat result: %#v\n", getmoreStat)
		if getmoreStat.OpType != gmHelperNames.getmoreOpType ||
			getmoreStat.NumReturned != 5 ||
			getmoreStat.Command != gmHelperNames.getmoreCommandName ||
			getmoreStat.Ns != gmHelperNames.namespace {
			t.Errorf("Getmore Not matched.Expected OpType:%s CommandName:%s NumReturned:%d Ns:%s --- Found OpType:%s CommandName:%s NumReturned: %d Ns: %s\n",
				gmHelperNames.getmoreOpType, gmHelperNames.getmoreCommandName, 5, gmHelperNames.namespace, getmoreStat.OpType, getmoreStat.Command, getmoreStat.NumReturned, getmoreStat.Ns)
		}
	}
}

func teardownDB() error {
	session, err := mgo.DialWithTimeout(currentTestURL, 30*time.Second)
	if err != nil {
		return err
	}

	session.DB(testDB).C(testCollection).DropCollection()
	session.Close()
	return nil
}
