// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package db

import (
	mgo "gopkg.in/mgo.v2"
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
type CommandRunner interface {
	Run(command interface{}, out interface{}, database string) error
	FindOne(db, collection string, skip int, query interface{}, sort []string, into interface{}, opts int) error
	Remove(db, collection string, query interface{}) error
	DatabaseNames() ([]string, error)
	CollectionNames(db string) ([]string, error)
}

// Remove removes all documents matched by query q in the db database and c collection.
func (sp *SessionProvider) Remove(db, c string, q interface{}) error {
	session, err := sp.GetSession()
	if err != nil {
		return err
	}
	defer session.Close()
	_, err = session.DB(db).C(c).RemoveAll(q)
	return err
}

// Run issues the provided command on the db database and unmarshals its result
// into out.
func (sp *SessionProvider) Run(command interface{}, out interface{}, db string) error {
	session, err := sp.GetSession()
	if err != nil {
		return err
	}
	defer session.Close()
	return session.DB(db).Run(command, out)
}

// DatabaseNames returns a slice containing the names of all the databases on the
// connected server.
func (sp *SessionProvider) DatabaseNames() ([]string, error) {
	session, err := sp.GetSession()
	if err != nil {
		return nil, err
	}
	session.SetSocketTimeout(0)
	defer session.Close()
	return session.DatabaseNames()
}

// CollectionNames returns the names of all the collections in the dbName database.
func (sp *SessionProvider) CollectionNames(dbName string) ([]string, error) {
	session, err := sp.GetSession()
	if err != nil {
		return nil, err
	}
	defer session.Close()
	session.SetSocketTimeout(0)
	return session.DB(dbName).CollectionNames()
}

// GetNodeType checks if the connected SessionProvider is a mongos, standalone, or replset,
// by looking at the result of calling isMaster.
func (sp *SessionProvider) GetNodeType() (NodeType, error) {
	session, err := sp.GetSession()
	if err != nil {
		return Unknown, err
	}
	session.SetSocketTimeout(0)
	defer session.Close()
	masterDoc := struct {
		SetName interface{} `bson:"setName"`
		Hosts   interface{} `bson:"hosts"`
		Msg     string      `bson:"msg"`
	}{}
	err = session.Run("isMaster", &masterDoc)
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

// SupportsCollectionUUID returns true if the connected server identifies
// collections with UUIDs
func (sp *SessionProvider) SupportsCollectionUUID() (bool, error) {
	session, err := sp.GetSession()
	if err != nil {
		return false, err
	}
	defer session.Close()

	collInfo, err := GetCollectionInfo(session.DB("admin").C("system.version"))
	if err != nil {
		return false, err
	}

	// On FCV 3.6+, admin.system.version will have a UUID
	if collInfo != nil && collInfo.GetUUID() != "" {
		return true, nil
	}

	return false, nil
}

// SupportsWriteCommands returns true if the connected server supports write
// commands, returns false otherwise.
func (sp *SessionProvider) SupportsWriteCommands() (bool, error) {
	session, err := sp.GetSession()
	if err != nil {
		return false, err
	}
	session.SetSocketTimeout(0)
	defer session.Close()
	masterDoc := struct {
		Ok      int `bson:"ok"`
		MaxWire int `bson:"maxWireVersion"`
	}{}
	err = session.Run("isMaster", &masterDoc)
	if err != nil {
		return false, err
	}
	// the connected server supports write commands if
	// the maxWriteVersion field is present
	return (masterDoc.Ok == 1 && masterDoc.MaxWire >= 2), nil
}

// FindOne returns the first document in the collection and database that matches
// the query after skip, sort and query flags are applied.
func (sp *SessionProvider) FindOne(db, collection string, skip int, query interface{}, sort []string, into interface{}, flags int) error {
	session, err := sp.GetSession()
	if err != nil {
		return err
	}
	defer session.Close()

	q := session.DB(db).C(collection).Find(query).Sort(sort...).Skip(skip)
	q = ApplyFlags(q, session, flags)
	return q.One(into)
}

// ApplyFlags applies flags to the given query session.
func ApplyFlags(q *mgo.Query, session *mgo.Session, flags int) *mgo.Query {
	if flags&Snapshot > 0 {
		q = q.Hint("_id")
	}
	if flags&LogReplay > 0 {
		q = q.LogReplay()
	}
	if flags&Prefetch > 0 {
		session.SetPrefetch(1.0)
	}
	return q
}
