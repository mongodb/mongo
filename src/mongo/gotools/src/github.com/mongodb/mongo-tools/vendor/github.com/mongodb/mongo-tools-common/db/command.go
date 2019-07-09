// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package db

import (
	"context"
	"fmt"

	"go.mongodb.org/mongo-driver/bson"
	mopt "go.mongodb.org/mongo-driver/mongo/options"
)

// Query flags
const (
	Snapshot = 1 << iota
	LogReplay
	Prefetch
)

type NodeType string

const (
	Mongos     NodeType = "mongos"
	Standalone          = "standalone"
	ReplSet             = "replset"
	Unknown             = "unknown"
)

// CommandRunner exposes functions that can be run against a server
// XXX Does anything rely on this?
type CommandRunner interface {
	Run(command interface{}, out interface{}, database string) error
	RunString(commandName string, out interface{}, database string) error
	FindOne(db, collection string, skip int, query interface{}, sort []string, into interface{}, opts int) error
	Remove(db, collection string, query interface{}) error
	DatabaseNames() ([]string, error)
	CollectionNames(db string) ([]string, error)
}

// // Remove removes all documents matched by query q in the db database and c collection.
// func (sp *SessionProvider) Remove(db, c string, q interface{}) error {
// 	session, err := sp.GetSession()
// 	if err != nil {
// 		return err
// 	}
// 	_, err = session.Database(db).Collection(c).RemoveAll(q)
// 	return err
// }
//
// Run issues the provided command on the db database and unmarshals its result
// into out.

func (sp *SessionProvider) Run(command interface{}, out interface{}, name string) error {
	db := sp.DB(name)
	result := db.RunCommand(context.Background(), command)
	if result.Err() != nil {
		return result.Err()
	}
	err := result.Decode(out)
	if err != nil {
		return err
	}
	return nil
}

func (sp *SessionProvider) RunString(commandName string, out interface{}, name string) error {
	command := &bson.M{commandName: 1}
	return sp.Run(command, out, name)
}

func (sp *SessionProvider) DropDatabase(dbName string) error {
	return sp.DB(dbName).Drop(context.Background())
}

func (sp *SessionProvider) CreateCollection(dbName, collName string) error {
	command := &bson.M{"create": collName}
	out := &bson.Raw{}
	err := sp.Run(command, out, dbName)
	return err
}

func (sp *SessionProvider) ServerVersion() (string, error) {
	out := struct{ Version string }{}
	err := sp.RunString("buildInfo", &out, "admin")
	if err != nil {
		return "", err
	}
	return out.Version, nil
}

func (sp *SessionProvider) ServerVersionArray() (Version, error) {
	var version Version
	out := struct {
		VersionArray []int32 `bson:"versionArray"`
	}{}
	err := sp.RunString("buildInfo", &out, "admin")
	if err != nil {
		return version, fmt.Errorf("error getting buildInfo: %v", err)
	}
	if len(out.VersionArray) < 3 {
		return version, fmt.Errorf("buildInfo.versionArray had fewer than 3 elements")
	}
	for i := 0; i <= 2; i++ {
		version[i] = int(out.VersionArray[i])
	}
	return version, nil
}

// DatabaseNames returns a slice containing the names of all the databases on the
// connected server.
func (sp *SessionProvider) DatabaseNames() ([]string, error) {
	return sp.client.ListDatabaseNames(nil, bson.D{})
}

// CollectionNames returns the names of all the collections in the dbName database.
// func (sp *SessionProvider) CollectionNames(dbName string) ([]string, error) {
// 	session, err := sp.GetSession()
// 	if err != nil {
// 		return nil, err
// 	}
// 	return session.DB(dbName).CollectionNames()
// }

// GetNodeType checks if the connected SessionProvider is a mongos, standalone, or replset,
// by looking at the result of calling isMaster.
func (sp *SessionProvider) GetNodeType() (NodeType, error) {
	session, err := sp.GetSession()
	if err != nil {
		return Unknown, err
	}
	masterDoc := struct {
		SetName interface{} `bson:"setName"`
		Hosts   interface{} `bson:"hosts"`
		Msg     string      `bson:"msg"`
	}{}
	result := session.Database("admin").RunCommand(
		context.Background(),
		&bson.M{"ismaster": 1},
	)
	if result.Err() != nil {
		return Unknown, result.Err()
	}
	err = result.Decode(&masterDoc)
	if err != nil {
		return Unknown, err
	}
	if masterDoc.SetName != nil || masterDoc.Hosts != nil {
		return ReplSet, nil
	} else if masterDoc.Msg == "isdbgrid" {
		// isdbgrid is always the msg value when calling isMaster on a mongos
		// see http://docs.mongodb.org/manual/core/sharded-cluster-query-router/
		return Mongos, nil
	}
	return Standalone, nil
}

// IsReplicaSet returns a boolean which is true if the connected server is part
// of a replica set.
func (sp *SessionProvider) IsReplicaSet() (bool, error) {
	nodeType, err := sp.GetNodeType()
	if err != nil {
		return false, err
	}
	return nodeType == ReplSet, nil
}

// IsMongos returns true if the connected server is a mongos.
func (sp *SessionProvider) IsMongos() (bool, error) {
	nodeType, err := sp.GetNodeType()
	if err != nil {
		return false, err
	}
	return nodeType == Mongos, nil
}

//
// // SupportsCollectionUUID returns true if the connected server identifies
// // collections with UUIDs
// func (sp *SessionProvider) SupportsCollectionUUID() (bool, error) {
// 	session, err := sp.GetSession()
// 	if err != nil {
// 		return false, err
// 	}
//
// 	collInfo, err := GetCollectionInfo(session.Database("admin").Collection("system.version"))
// 	if err != nil {
// 		return false, err
// 	}
//
// 	// On FCV 3.6+, admin.system.version will have a UUID
// 	if collInfo != nil && collInfo.GetUUID() != "" {
// 		return true, nil
// 	}
//
// 	return false, nil
// }

//
// // SupportsWriteCommands returns true if the connected server supports write
// // commands, returns false otherwise.
// func (sp *SessionProvider) SupportsWriteCommands() (bool, error) {
// 	session, err := sp.GetSession()
// 	if err != nil {
// 		return false, err
// 	}
// 	masterDoc := struct {
// 		Ok      int `bson:"ok"`
// 		MaxWire int `bson:"maxWireVersion"`
// 	}{}
// 	err = session.Run("isMaster", &masterDoc)
// 	if err != nil {
// 		return false, err
// 	}
// 	// the connected server supports write commands if
// 	// the maxWriteVersion field is present
// 	return (masterDoc.Ok == 1 && masterDoc.MaxWire >= 2), nil
// }

// FindOne retuns the first document in the collection and database that matches
// the query after skip, sort and query flags are applied.
func (sp *SessionProvider) FindOne(db, collection string, skip int, query interface{}, sort interface{}, into interface{}, flags int) error {
	session, err := sp.GetSession()
	if err != nil {
		return err
	}

	if query == nil {
		query = bson.D{}
	}

	opts := mopt.FindOne().SetSort(sort).SetSkip(int64(skip))
	ApplyFlags(opts, flags)

	res := session.Database(db).Collection(collection).FindOne(nil, query, opts)
	err = res.Decode(into)
	return err
}

// ApplyFlags applies flags to the given query session.
func ApplyFlags(opts *mopt.FindOneOptions, flags int) {
	if flags&Snapshot > 0 {
		opts.SetHint(bson.D{{"_id", 1}})
	}
	if flags&LogReplay > 0 {
		opts.SetOplogReplay(true)
	}
}
