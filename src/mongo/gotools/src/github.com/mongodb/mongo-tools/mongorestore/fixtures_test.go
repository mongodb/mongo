// Copyright (C) MongoDB, Inc. 2019-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongorestore

import (
	"fmt"
	"os"
	"path"
	"path/filepath"
	"strings"

	"github.com/mongodb/mongo-tools-common/db"
	"github.com/mongodb/mongo-tools/legacy/util"
	"go.mongodb.org/mongo-driver/bson"
)

type testCollData struct {
	ns       string
	docs     []bson.D
	metadata bson.D
}

func (tcd testCollData) SplitNS() (string, string) {
	ns := strings.SplitN(tcd.ns, ".", 2)
	if len(ns) != 2 {
		panic(fmt.Sprintf("invalid namespace '%s'", tcd.ns))
	}
	return ns[0], util.EscapeCollectionName(ns[1])
}

func (tcd testCollData) Mkdir(basePath string) error {
	db, _ := tcd.SplitNS()
	dbDir := path.Join(basePath, db)
	err := os.MkdirAll(dbDir, 0755)
	if err != nil {
		return err
	}
	return nil
}

func (tcd testCollData) WriteData(basePath string) error {
	db, coll := tcd.SplitNS()
	file, err := os.Create(path.Join(basePath, db, coll+".bson"))
	if err != nil {
		return err
	}
	defer file.Close()

	for _, doc := range tcd.docs {
		raw, err := bson.Marshal(doc)
		if err != nil {
			return err
		}
		_, err = file.Write(raw)
		if err != nil {
			return err
		}
	}

	return file.Sync()
}

func (tcd testCollData) WriteMetadata(basePath string) error {
	if tcd.metadata == nil {
		return nil
	}

	db, coll := tcd.SplitNS()
	file, err := os.Create(path.Join(basePath, db, coll+".metadata.json"))
	if err != nil {
		return err
	}
	defer file.Close()

	raw, err := bson.MarshalExtJSON(tcd.metadata, true, false)
	if err != nil {
		return err
	}
	_, err = file.Write(raw)
	if err != nil {
		return err
	}

	return file.Sync()
}

type testDumpDir struct {
	dirName     string
	oplog       []db.Oplog
	collections []testCollData
}

func (tdd *testDumpDir) Create() error {

	err := tdd.Cleanup()
	if err != nil {
		return err
	}

	err = os.MkdirAll(tdd.Path(), 0755)
	if err != nil {
		return err
	}

	for _, coll := range tdd.collections {
		err := coll.Mkdir(tdd.Path())
		if err != nil {
			return err
		}
		err = coll.WriteData(tdd.Path())
		if err != nil {
			return err
		}
		err = coll.WriteMetadata(tdd.Path())
		if err != nil {
			return err
		}
	}

	err = tdd.WriteOplog()
	if err != nil {
		return err
	}

	return nil
}

// Cleanup removes the test directory unless "NO_CLEANUP" is set in the environment.
func (tdd *testDumpDir) Cleanup() error {
	noCleanup := os.Getenv("NO_CLEANUP")
	if noCleanup == "0" {
		return nil
	}
	return os.RemoveAll(tdd.Path())
}

func (tdd *testDumpDir) WriteOplog() error {
	if tdd.oplog == nil {
		return nil
	}
	file, err := os.Create(path.Join(tdd.Path(), "oplog.bson"))
	if err != nil {
		return err
	}
	defer file.Close()

	for _, op := range tdd.oplog {
		raw, err := bson.Marshal(op)
		if err != nil {
			return err
		}
		_, err = file.Write(raw)
		if err != nil {
			return err
		}
	}

	return file.Sync()
}

func (tdd *testDumpDir) Path() string {
	return filepath.Join("testdata", tdd.dirName)
}
