// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package db

import (
	"context"
	"encoding/hex"
	"fmt"
	"strings"

	"github.com/mongodb/mongo-tools-common/log"
	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/primitive"
	"go.mongodb.org/mongo-driver/mongo"
)

type CollectionInfo struct {
	Name    string  `bson:"name"`
	Type    string  `bson:"type"`
	Options bson.M `bson:"options"`
	Info    bson.M `bson:"info"`
}

func (ci *CollectionInfo) IsView() bool {
	return ci.Type == "view"
}

func (ci *CollectionInfo) IsSystemCollection() bool {
	return strings.HasPrefix(ci.Name, "system.")
}

func (ci *CollectionInfo) GetUUID() string {
	if ci.Info == nil {
		return ""
	}
	if v, ok := ci.Info["uuid"]; ok {
		switch x := v.(type) {
		case primitive.Binary:
			if x.Subtype == 4 {
				return hex.EncodeToString(x.Data)
			}
		default:
			log.Logvf(log.DebugHigh, "unknown UUID BSON type '%T'", v)
		}
	}
	return ""
}

// GetIndexes returns an iterator to thethe raw index info for a collection by
// using the listIndexes command if available, or by falling back to querying
// against system.indexes (pre-3.0 systems). nil is returned if the collection
// does not exist.
func GetIndexes(coll *mongo.Collection) (*mongo.Cursor, error) {
	return coll.Indexes().List(context.Background())
}

// Assumes that mongo.Database will normalize legacy names to omit database
// name as required by the Enumerate Collections spec
func GetCollections(database *mongo.Database, name string) (*mongo.Cursor, error) {
	filter := bson.D{}
	if len(name) > 0 {
		filter = append(filter, primitive.E{"name", name})
	}

	cursor, err := database.ListCollections(nil, filter)
	if err != nil {
		return nil, err
	}

	return cursor, nil
}

func GetCollectionInfo(coll *mongo.Collection) (*CollectionInfo, error) {
	iter, err := GetCollections(coll.Database(), coll.Name())
	if err != nil {
		return nil, err
	}
	defer iter.Close(context.Background())
	comparisonName := coll.Name()

	var foundCollInfo *CollectionInfo
	for iter.Next(nil) {
		collInfo := &CollectionInfo{}
		err = iter.Decode(collInfo)
		if err != nil {
			return nil, err
		}
		if collInfo.Name == comparisonName {
			foundCollInfo = collInfo
			break
		}
	}
	if err := iter.Err(); err != nil {
		return nil, err
	}
	return foundCollInfo, nil
}

func StripDBFromNamespace(namespace string, dbName string) (string, error) {
	namespacePrefix := dbName + "."
	// if the collection info came from querying system.indexes (2.6 or earlier) then the
	// "name" we get includes the db name as well, so we must remove it
	if strings.HasPrefix(namespace, namespacePrefix) {
		return namespace[len(namespacePrefix):], nil
	}
	return "", fmt.Errorf("namespace '%v' format is invalid - expected to start with '%v'", namespace, namespacePrefix)
}
