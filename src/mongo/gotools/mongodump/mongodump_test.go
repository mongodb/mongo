// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongodump

import (
	"bytes"
	"fmt"
	"io/ioutil"
	"math/rand"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"

	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/json"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"
	"github.com/mongodb/mongo-tools/common/util"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
)

var (
	// database with test data
	testDB = "mongodump_test_db"
	// temp database used for restoring a DB
	testRestoreDB       = "temp_mongodump_restore_test_db"
	testCollectionNames = []string{"coll1", "coll2", "coll3"}
)

const (
	KerberosDumpDirectory = "dump-kerberos"
)

func simpleMongoDumpInstance() *MongoDump {
	var toolOptions *options.ToolOptions

	// get ToolOptions from URI or defaults
	if uri := os.Getenv("MONGOD"); uri != "" {
		fakeArgs := []string{"--uri=" + uri}
		toolOptions = options.New("mongodump", "", options.EnabledOptions{URI: true})
		toolOptions.URI.AddKnownURIParameters(options.KnownURIOptionsReadPreference)
		_, err := toolOptions.ParseArgs(fakeArgs)
		if err != nil {
			panic("Could not parse MONGOD environment variable")
		}
	} else {
		ssl := testutil.GetSSLOptions()
		auth := testutil.GetAuthOptions()
		connection := &options.Connection{
			Host: "localhost",
			Port: db.DefaultTestPort,
		}
		toolOptions = &options.ToolOptions{
			SSL:        &ssl,
			Connection: connection,
			Auth:       &auth,
			Verbosity:  &options.Verbosity{},
		}
	}

	// Limit ToolOptions to test database
	toolOptions.Namespace = &options.Namespace{DB: testDB}

	outputOptions := &OutputOptions{
		NumParallelCollections: 1,
	}
	inputOptions := &InputOptions{}

	log.SetVerbosity(toolOptions.Verbosity)

	return &MongoDump{
		ToolOptions:   toolOptions,
		InputOptions:  inputOptions,
		OutputOptions: outputOptions,
	}
}

// returns the number of .bson files in a directory
// excluding system.indexes.bson
func countNonIndexBSONFiles(dir string) (int, error) {
	matchingFiles, err := getMatchingFiles(dir, ".*\\.bson")
	if err != nil {
		return 0, err
	}
	count := 0
	for _, fileName := range matchingFiles {
		if fileName != "system.indexes.bson" {
			count++
		}
	}
	return count, nil
}

// returns count of metadata files
func countMetaDataFiles(dir string) (int, error) {
	matchingFiles, err := getMatchingFiles(dir, ".*\\.metadata\\.json")
	if err != nil {
		return 0, err
	}
	return len(matchingFiles), nil
}

// returns count of oplog entries with 'ui' field
func countOplogUI(iter *db.DecodedBSONSource) int {
	var count int
	var doc bson.M
	for iter.Next(&doc) {
		count += countOpsWithUI(doc)
	}
	return count
}

func countOpsWithUI(doc bson.M) int {
	var count int
	switch doc["op"] {
	case "i", "u", "d":
		if _, ok := doc["ui"]; ok {
			count++
		}
	case "c":
		if _, ok := doc["ui"]; ok {
			count++
		} else if v, ok := doc["o"]; ok {
			opts, _ := v.(bson.M)
			if applyOps, ok := opts["applyOps"]; ok {
				list := applyOps.([]bson.M)
				for _, v := range list {
					count += countOpsWithUI(v)
				}
			}
		}
	}
	return count
}

// returns filenames that match the given pattern
func getMatchingFiles(dir, pattern string) ([]string, error) {
	fileInfos, err := ioutil.ReadDir(dir)
	if err != nil {
		return nil, err
	}

	matchingFiles := []string{}
	var matched bool
	for _, fileInfo := range fileInfos {
		fileName := fileInfo.Name()
		if matched, err = regexp.MatchString(pattern, fileName); matched {
			matchingFiles = append(matchingFiles, fileName)
		}
		if err != nil {
			return nil, err
		}
	}
	return matchingFiles, nil
}

// read all the database bson documents from dir and put it into another DB
// ignore the inddexes for now
func readBSONIntoDatabase(dir, restoreDBName string) error {
	if ok := fileDirExists(dir); !ok {
		return fmt.Errorf("error finding '%v' on local FS", dir)
	}

	session, err := testutil.GetBareSession()
	if err != nil {
		return err
	}
	defer session.Close()

	fileInfos, err := ioutil.ReadDir(dir)
	if err != nil {
		return err
	}

	for _, fileInfo := range fileInfos {
		fileName := fileInfo.Name()
		if !strings.HasSuffix(fileName, ".bson") || fileName == "system.indexes.bson" {
			continue
		}

		collectionName := fileName[:strings.LastIndex(fileName, ".bson")]
		collection := session.DB(restoreDBName).C(collectionName)

		file, err := os.Open(fmt.Sprintf("%s/%s", dir, fileName))
		if err != nil {
			return err
		}
		defer file.Close()

		bsonSource := db.NewDecodedBSONSource(db.NewBSONSource(file))
		defer bsonSource.Close()

		var result bson.M
		for bsonSource.Next(&result) {
			err = collection.Insert(result)
			if err != nil {
				return err
			}
		}
		if err = bsonSource.Err(); err != nil {
			return err
		}
	}

	return nil
}

func setUpMongoDumpTestData() error {
	session, err := testutil.GetBareSession()
	if err != nil {
		return err
	}
	defer session.Close()

	for i, collectionName := range testCollectionNames {
		coll := session.DB(testDB).C(collectionName)

		for j := 0; j < 10*(i+1); j++ {
			err = coll.Insert(bson.M{"collectionName": collectionName, "age": j})
			if err != nil {
				return err
			}
		}
	}

	return nil
}

// backgroundInsert inserts into random collections until provided done
// channel is closed.  The function closes the ready channel to signal that
// background insertion has started.  When the done channel is closed, the
// function returns.  Any errors are passed back on the errs channel.
func backgroundInsert(ready, done chan struct{}, errs chan error) {
	defer close(errs)
	session, err := testutil.GetBareSession()
	if err != nil {
		errs <- err
		close(ready)
		return
	}
	defer session.Close()

	colls := make([]*mgo.Collection, len(testCollectionNames))
	for i, v := range testCollectionNames {
		colls[i] = session.DB(testDB).C(v)
	}

	var n int

	// Insert a doc to ensure the DB is actually ready for inserts
	// and not pausing while a dropDatabase is processing.
	err = colls[0].Insert(bson.M{"n": n})
	if err != nil {
		errs <- err
		close(ready)
		return
	}
	close(ready)
	n++

	for {
		select {
		case <-done:
			return
		default:
			coll := colls[rand.Intn(len(colls))]
			err := coll.Insert(bson.M{"n": n})
			if err != nil {
				errs <- err
				return
			}
			n++
		}
	}
}

func tearDownMongoDumpTestData() error {
	session, err := testutil.GetBareSession()
	if err != nil {
		return err
	}
	defer session.Close()

	err = session.DB(testDB).DropDatabase()
	if err != nil {
		return err
	}
	return nil
}

func fileDirExists(name string) bool {
	if _, err := os.Stat(name); err != nil {
		if os.IsNotExist(err) {
			return false
		}
	}
	return true
}

func testQuery(md *MongoDump, session *mgo.Session) string {
	origDB := session.DB(testDB)
	restoredDB := session.DB(testRestoreDB)

	// query to test --query* flags
	bsonQuery := bson.M{"age": bson.M{"$lt": 10}}

	// we can only dump using query per collection
	for _, testCollName := range testCollectionNames {
		md.ToolOptions.Namespace.Collection = testCollName

		err := md.Init()
		So(err, ShouldBeNil)

		err = md.Dump()
		So(err, ShouldBeNil)
	}

	path, err := os.Getwd()
	So(err, ShouldBeNil)

	dumpDir := util.ToUniversalPath(filepath.Join(path, "dump"))
	dumpDBDir := util.ToUniversalPath(filepath.Join(dumpDir, testDB))
	So(fileDirExists(dumpDir), ShouldBeTrue)
	So(fileDirExists(dumpDBDir), ShouldBeTrue)

	err = readBSONIntoDatabase(dumpDBDir, testRestoreDB)
	So(err, ShouldBeNil)

	for _, testCollName := range testCollectionNames {
		// count filtered docs
		numDocs1, err := origDB.C(testCollName).Find(bsonQuery).Count()
		So(err, ShouldBeNil)

		// count number of all restored documents
		numDocs2, err := restoredDB.C(testCollName).Find(nil).Count()
		So(err, ShouldBeNil)

		So(numDocs1, ShouldEqual, numDocs2)
	}
	return dumpDir
}

func TestMongoDumpValidateOptions(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("With a MongoDump instance", t, func() {
		md := simpleMongoDumpInstance()

		Convey("we cannot dump a collection when a database specified", func() {
			md.ToolOptions.Namespace.Collection = "some_collection"
			md.ToolOptions.Namespace.DB = ""

			err := md.Init()
			So(err, ShouldNotBeNil)
			So(err.Error(), ShouldContainSubstring, "cannot dump a collection without a specified database")
		})

		Convey("we have to specify a collection name if using a query", func() {
			md.ToolOptions.Namespace.Collection = ""
			md.OutputOptions.Out = ""
			md.InputOptions.Query = "{_id:\"\"}"

			err := md.Init()
			So(err, ShouldNotBeNil)
			So(err.Error(), ShouldContainSubstring, "cannot dump using a query without a specified collection")
		})

	})
}

func TestMongoDumpKerberos(t *testing.T) {
	testutil.VerifyTestType(t, testutil.KerberosTestType)

	Convey("Should be able to run mongodump with Kerberos auth", t, func() {
		opts, err := testutil.GetKerberosOptions()

		So(err, ShouldBeNil)

		mongoDump := MongoDump{
			ToolOptions:  opts,
			InputOptions: &InputOptions{},
			OutputOptions: &OutputOptions{
				NumParallelCollections: 1,
			},
		}

		mongoDump.OutputOptions.Out = KerberosDumpDirectory

		err = mongoDump.Init()
		So(err, ShouldBeNil)
		err = mongoDump.Dump()
		So(err, ShouldBeNil)
		path, err := os.Getwd()
		So(err, ShouldBeNil)

		dumpDir := util.ToUniversalPath(filepath.Join(path, KerberosDumpDirectory))
		dumpDBDir := util.ToUniversalPath(filepath.Join(dumpDir, opts.Namespace.DB))
		So(fileDirExists(dumpDir), ShouldBeTrue)
		So(fileDirExists(dumpDBDir), ShouldBeTrue)

		dumpCollectionFile := util.ToUniversalPath(filepath.Join(dumpDBDir, opts.Namespace.Collection+".bson"))
		So(fileDirExists(dumpCollectionFile), ShouldBeTrue)

		countColls, err := countNonIndexBSONFiles(dumpDBDir)
		So(err, ShouldBeNil)
		So(countColls, ShouldEqual, 1)
	})
}

func TestMongoDumpBSON(t *testing.T) {
	testutil.VerifyTestType(t, testutil.IntegrationTestType)
	log.SetWriter(ioutil.Discard)

	Convey("With a MongoDump instance", t, func() {
		err := setUpMongoDumpTestData()
		So(err, ShouldBeNil)

		Convey("testing that using MongoDump WITHOUT giving a query dumps everything in the database and/or collection", func() {
			md := simpleMongoDumpInstance()
			md.InputOptions.Query = ""

			Convey("and that for a particular collection", func() {
				md.ToolOptions.Namespace.Collection = testCollectionNames[0]
				err = md.Init()
				So(err, ShouldBeNil)

				Convey("it dumps to the default output directory", func() {
					// we don't have to set this manually if parsing options via command line
					md.OutputOptions.Out = "dump"
					err = md.Dump()
					So(err, ShouldBeNil)
					path, err := os.Getwd()
					So(err, ShouldBeNil)

					dumpDir := util.ToUniversalPath(filepath.Join(path, "dump"))
					dumpDBDir := util.ToUniversalPath(filepath.Join(dumpDir, testDB))
					So(fileDirExists(dumpDir), ShouldBeTrue)
					So(fileDirExists(dumpDBDir), ShouldBeTrue)

					err = readBSONIntoDatabase(dumpDBDir, testRestoreDB)
					So(err, ShouldBeNil)

					session, err := testutil.GetBareSession()
					So(err, ShouldBeNil)

					countColls, err := countNonIndexBSONFiles(dumpDBDir)
					So(err, ShouldBeNil)
					So(countColls, ShouldEqual, 1)

					collOriginal := session.DB(testDB).C(testCollectionNames[0])
					collRestore := session.DB(testRestoreDB).C(testCollectionNames[0])

					Convey("with the correct number of documents", func() {
						numDocsOrig, err := collOriginal.Count()
						So(err, ShouldBeNil)

						numDocsRestore, err := collRestore.Count()
						So(err, ShouldBeNil)

						So(numDocsOrig, ShouldEqual, numDocsRestore)
					})

					Convey("that are the same as the documents in the test database", func() {
						iter := collOriginal.Find(nil).Iter()

						var result bson.M
						for iter.Next(&result) {
							restoredCount, err := collRestore.Find(result).Count()
							So(err, ShouldBeNil)
							So(restoredCount, ShouldNotEqual, 0)
						}
						So(iter.Close(), ShouldBeNil)
					})

					Reset(func() {
						So(session.DB(testRestoreDB).DropDatabase(), ShouldBeNil)
						So(os.RemoveAll(dumpDir), ShouldBeNil)
					})
				})

				Convey("it dumps to a user-specified output directory", func() {
					md.OutputOptions.Out = "dump_user"
					err = md.Dump()
					So(err, ShouldBeNil)
					path, err := os.Getwd()
					So(err, ShouldBeNil)

					dumpDir := util.ToUniversalPath(filepath.Join(path, "dump_user"))
					dumpDBDir := util.ToUniversalPath(filepath.Join(dumpDir, testDB))
					So(fileDirExists(dumpDir), ShouldBeTrue)
					So(fileDirExists(dumpDBDir), ShouldBeTrue)

					countColls, err := countNonIndexBSONFiles(dumpDBDir)
					So(err, ShouldBeNil)
					So(countColls, ShouldEqual, 1)

					Reset(func() {
						So(os.RemoveAll(dumpDir), ShouldBeNil)
					})

				})

				Convey("it dumps to standard output", func() {
					md.OutputOptions.Out = "-"
					stdoutBuf := &bytes.Buffer{}
					md.OutputWriter = stdoutBuf
					err = md.Dump()
					So(err, ShouldBeNil)
					var count int
					bsonSource := db.NewDecodedBSONSource(db.NewBSONSource(ioutil.NopCloser(stdoutBuf)))
					defer bsonSource.Close()

					var result bson.Raw
					for bsonSource.Next(&result) {
						count++
					}
					So(bsonSource.Err(), ShouldBeNil)
					So(count, ShouldEqual, 10) //The 0th collection has 10 documents

					Reset(func() {
					})

				})

			})

			Convey("for an entire database", func() {
				md.ToolOptions.Namespace.Collection = ""
				err = md.Init()
				So(err, ShouldBeNil)

				Convey("that exists. The dumped directory should contain the necessary bson files", func() {
					md.OutputOptions.Out = "dump"
					err = md.Dump()
					So(err, ShouldBeNil)
					path, err := os.Getwd()
					So(err, ShouldBeNil)

					dumpDir := util.ToUniversalPath(filepath.Join(path, "dump"))
					dumpDBDir := util.ToUniversalPath(filepath.Join(dumpDir, testDB))
					So(fileDirExists(dumpDir), ShouldBeTrue)
					So(fileDirExists(dumpDBDir), ShouldBeTrue)

					countColls, err := countNonIndexBSONFiles(dumpDBDir)
					So(err, ShouldBeNil)
					So(countColls, ShouldEqual, len(testCollectionNames))

					Reset(func() {
						So(os.RemoveAll(dumpDir), ShouldBeNil)
					})

				})

				Convey("that does not exist. The dumped directory shouldn't be created", func() {
					md.OutputOptions.Out = "dump"
					md.ToolOptions.Namespace.DB = "nottestdb"
					err = md.Dump()
					So(err, ShouldBeNil)

					path, err := os.Getwd()
					So(err, ShouldBeNil)

					dumpDir := util.ToUniversalPath(filepath.Join(path, "dump"))
					dumpDBDir := util.ToUniversalPath(filepath.Join(dumpDir, "nottestdb"))

					So(fileDirExists(dumpDir), ShouldBeFalse)
					So(fileDirExists(dumpDBDir), ShouldBeFalse)
				})

			})
		})

		Convey("testing that using MongoDump WITH a query dumps a subset of documents in a database and/or collection", func() {
			session, err := testutil.GetBareSession()
			So(err, ShouldBeNil)
			md := simpleMongoDumpInstance()

			// expect 10 documents per collection
			bsonQuery := bson.M{"age": bson.M{"$lt": 10}}
			jsonQuery, err := bsonutil.ConvertBSONValueToJSON(bsonQuery)
			So(err, ShouldBeNil)
			jsonQueryBytes, err := json.Marshal(jsonQuery)
			So(err, ShouldBeNil)

			Convey("using --query for all the collections in the database", func() {
				md.InputOptions.Query = string(jsonQueryBytes)
				md.ToolOptions.Namespace.DB = testDB
				md.OutputOptions.Out = "dump"
				dumpDir := testQuery(md, session)

				Reset(func() {
					So(session.DB(testRestoreDB).DropDatabase(), ShouldBeNil)
					So(os.RemoveAll(dumpDir), ShouldBeNil)
				})

			})

			Convey("using --queryFile for all the collections in the database", func() {
				ioutil.WriteFile("example.json", jsonQueryBytes, 0777)
				md.InputOptions.QueryFile = "example.json"
				md.ToolOptions.Namespace.DB = testDB
				md.OutputOptions.Out = "dump"
				dumpDir := testQuery(md, session)

				Reset(func() {
					So(session.DB(testRestoreDB).DropDatabase(), ShouldBeNil)
					So(os.RemoveAll(dumpDir), ShouldBeNil)
					So(os.Remove("example.json"), ShouldBeNil)
				})

			})
		})

		Reset(func() {
			So(tearDownMongoDumpTestData(), ShouldBeNil)
		})
	})
}

func TestMongoDumpMetaData(t *testing.T) {
	testutil.VerifyTestType(t, testutil.IntegrationTestType)
	log.SetWriter(ioutil.Discard)

	Convey("With a MongoDump instance", t, func() {
		session, err := testutil.GetBareSession()
		So(session, ShouldNotBeNil)
		So(err, ShouldBeNil)

		err = setUpMongoDumpTestData()
		So(err, ShouldBeNil)

		Convey("testing that the dumped directory contains information about indexes", func() {

			md := simpleMongoDumpInstance()
			md.OutputOptions.Out = "dump"
			err = md.Init()
			So(err, ShouldBeNil)

			err = md.Dump()
			So(err, ShouldBeNil)

			path, err := os.Getwd()
			So(err, ShouldBeNil)
			dumpDir := util.ToUniversalPath(filepath.Join(path, "dump"))
			dumpDBDir := util.ToUniversalPath(filepath.Join(dumpDir, testDB))
			So(fileDirExists(dumpDir), ShouldBeTrue)
			So(fileDirExists(dumpDBDir), ShouldBeTrue)

			Convey("having one metadata file per collection", func() {
				c1, err := countNonIndexBSONFiles(dumpDBDir)
				So(err, ShouldBeNil)

				c2, err := countMetaDataFiles(dumpDBDir)
				So(err, ShouldBeNil)

				So(c1, ShouldEqual, c2)

				Convey("and that the JSON in a metadata file is valid", func() {
					metaFiles, err := getMatchingFiles(dumpDBDir, ".*\\.metadata\\.json")
					So(err, ShouldBeNil)
					So(len(metaFiles), ShouldBeGreaterThan, 0)

					oneMetaFile, err := os.Open(util.ToUniversalPath(filepath.Join(dumpDBDir, metaFiles[0])))
					defer oneMetaFile.Close()
					So(err, ShouldBeNil)
					contents, err := ioutil.ReadAll(oneMetaFile)
					var jsonResult map[string]interface{}
					err = json.Unmarshal(contents, &jsonResult)
					So(err, ShouldBeNil)

					Convey("and contains an 'indexes' key", func() {
						_, ok := jsonResult["indexes"]
						So(ok, ShouldBeTrue)
					})

					fcv := testutil.GetFCV(session)
					cmp, err := testutil.CompareFCV(fcv, "3.6")
					So(err, ShouldBeNil)
					if cmp >= 0 {
						Convey("and on FCV 3.6+, contains a 'uuid' key", func() {
							uuid, ok := jsonResult["uuid"]
							So(ok, ShouldBeTrue)
							checkUUID := regexp.MustCompile(`(?i)^[a-z0-9]{32}$`)
							So(checkUUID.MatchString(uuid.(string)), ShouldBeTrue)
							So(err, ShouldBeNil)
						})
					}

				})

			})

			Reset(func() {
				So(os.RemoveAll(dumpDir), ShouldBeNil)
			})
		})

		Reset(func() {
			So(tearDownMongoDumpTestData(), ShouldBeNil)
		})

	})
}

func TestMongoDumpOplog(t *testing.T) {
	testutil.VerifyTestType(t, testutil.IntegrationTestType)
	session, err := testutil.GetBareSession()
	if err != nil {
		t.Fatalf("No server available")
	}
	if !testutil.IsReplicaSet(session) {
		t.SkipNow()
	}
	log.SetWriter(ioutil.Discard)

	Convey("With a MongoDump instance", t, func() {

		Convey("testing that the dumped directory contains an oplog", func() {

			// Start with clean filesystem
			path, err := os.Getwd()
			So(err, ShouldBeNil)

			dumpDir := util.ToUniversalPath(filepath.Join(path, "dump"))
			dumpOplogFile := util.ToUniversalPath(filepath.Join(dumpDir, "oplog.bson"))

			err = os.RemoveAll(dumpDir)
			So(err, ShouldBeNil)
			So(fileDirExists(dumpDir), ShouldBeFalse)

			// Start with clean database
			So(tearDownMongoDumpTestData(), ShouldBeNil)

			// Prepare mongodump with options
			md := simpleMongoDumpInstance()
			md.OutputOptions.Oplog = true
			md.ToolOptions.Namespace = &options.Namespace{}
			err = md.Init()
			So(err, ShouldBeNil)

			// Start inserting docs in the background so the oplog has data
			ready := make(chan struct{})
			done := make(chan struct{})
			errs := make(chan error, 1)
			go backgroundInsert(ready, done, errs)
			<-ready

			// Run mongodump
			err = md.Dump()
			So(err, ShouldBeNil)

			// Stop background insertion
			close(done)
			err = <-errs
			So(err, ShouldBeNil)

			// Check for and read the oplog file
			So(fileDirExists(dumpDir), ShouldBeTrue)
			So(fileDirExists(dumpOplogFile), ShouldBeTrue)

			oplogFile, err := os.Open(dumpOplogFile)
			defer oplogFile.Close()
			So(err, ShouldBeNil)

			rdr := db.NewBSONSource(oplogFile)
			iter := db.NewDecodedBSONSource(rdr)

			fcv := testutil.GetFCV(session)
			cmp, err := testutil.CompareFCV(fcv, "3.6")
			So(err, ShouldBeNil)

			withUI := countOplogUI(iter)

			if cmp >= 0 {
				// for FCV 3.6+, should have 'ui' field in oplog entries
				So(withUI, ShouldBeGreaterThan, 0)
			} else {
				// for FCV <3.6, should no have 'ui' field in oplog entries
				So(withUI, ShouldEqual, 0)
			}

			// Cleanup
			So(os.RemoveAll(dumpDir), ShouldBeNil)
			So(tearDownMongoDumpTestData(), ShouldBeNil)
		})

	})
}
