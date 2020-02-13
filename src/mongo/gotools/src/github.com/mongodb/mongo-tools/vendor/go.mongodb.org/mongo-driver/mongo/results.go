// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongo

import (
	"fmt"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/x/mongo/driver/operation"
)

// BulkWriteResult holds the result of a bulk write operation.
type BulkWriteResult struct {
	InsertedCount int64
	MatchedCount  int64
	ModifiedCount int64
	DeletedCount  int64
	UpsertedCount int64
	UpsertedIDs   map[int64]interface{}
}

// InsertOneResult is a result of an InsertOne operation.
//
// InsertedID will be a Go type that corresponds to a BSON type.
type InsertOneResult struct {
	// The identifier that was inserted.
	InsertedID interface{}
}

// InsertManyResult is a result of an InsertMany operation.
type InsertManyResult struct {
	// Maps the indexes of inserted documents to their _id fields.
	InsertedIDs []interface{}
}

// DeleteResult is a result of an DeleteOne operation.
type DeleteResult struct {
	// The number of documents that were deleted.
	DeletedCount int64 `bson:"n"`
}

// ListDatabasesResult is a result of a ListDatabases operation. Each specification
// is a description of the datbases on the server.
type ListDatabasesResult struct {
	Databases []DatabaseSpecification
	TotalSize int64
}

func newListDatabasesResultFromOperation(res operation.ListDatabasesResult) ListDatabasesResult {
	var ldr ListDatabasesResult
	ldr.Databases = make([]DatabaseSpecification, 0, len(res.Databases))
	for _, spec := range res.Databases {
		ldr.Databases = append(
			ldr.Databases,
			DatabaseSpecification{Name: spec.Name, SizeOnDisk: spec.SizeOnDisk, Empty: spec.Empty},
		)
	}
	ldr.TotalSize = res.TotalSize
	return ldr
}

// DatabaseSpecification is the information for a single database returned
// from a ListDatabases operation.
type DatabaseSpecification struct {
	Name       string
	SizeOnDisk int64
	Empty      bool
}

// UpdateResult is a result of an update operation.
//
// UpsertedID will be a Go type that corresponds to a BSON type.
type UpdateResult struct {
	// The number of documents that matched the filter.
	MatchedCount int64
	// The number of documents that were modified.
	ModifiedCount int64
	// The number of documents that were upserted.
	UpsertedCount int64
	// The identifier of the inserted document if an upsert took place.
	UpsertedID interface{}
}

// UnmarshalBSON implements the bson.Unmarshaler interface.
func (result *UpdateResult) UnmarshalBSON(b []byte) error {
	elems, err := bson.Raw(b).Elements()
	if err != nil {
		return err
	}

	for _, elem := range elems {
		switch elem.Key() {
		case "n":
			switch elem.Value().Type {
			case bson.TypeInt32:
				result.MatchedCount = int64(elem.Value().Int32())
			case bson.TypeInt64:
				result.MatchedCount = elem.Value().Int64()
			default:
				return fmt.Errorf("Received invalid type for n, should be Int32 or Int64, received %s", elem.Value().Type)
			}
		case "nModified":
			switch elem.Value().Type {
			case bson.TypeInt32:
				result.ModifiedCount = int64(elem.Value().Int32())
			case bson.TypeInt64:
				result.ModifiedCount = elem.Value().Int64()
			default:
				return fmt.Errorf("Received invalid type for nModified, should be Int32 or Int64, received %s", elem.Value().Type)
			}
		case "upserted":
			switch elem.Value().Type {
			case bson.TypeArray:
				e, err := elem.Value().Array().IndexErr(0)
				if err != nil {
					break
				}
				if e.Value().Type != bson.TypeEmbeddedDocument {
					break
				}
				var d struct {
					ID interface{} `bson:"_id"`
				}
				err = bson.Unmarshal(e.Value().Document(), &d)
				if err != nil {
					return err
				}
				result.UpsertedID = d.ID
			default:
				return fmt.Errorf("Received invalid type for upserted, should be Array, received %s", elem.Value().Type)
			}
		}
	}

	return nil
}
