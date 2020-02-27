// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mtest

import (
	"go.mongodb.org/mongo-driver/bson"
)

// BatchIdentifier specifies the keyword to identify the batch in a cursor response.
type BatchIdentifier string

// These constants specify valid values for BatchIdentifier.
const (
	FirstBatch BatchIdentifier = "firstBatch"
	NextBatch  BatchIdentifier = "nextBatch"
)

// CommandError is a representation of a command error from the server.
type CommandError struct {
	Code    int32
	Message string
	Name    string
	Labels  []string
}

// WriteError is a representation of a write error from the server.
type WriteError struct {
	Index   int
	Code    int
	Message string
}

// WriteConcernError is a representation of a write concern error from the server.
type WriteConcernError struct {
	Name    string   `bson:"codeName"`
	Code    int      `bson:"code"`
	Message string   `bson:"errmsg"`
	Details bson.Raw `bson:"errInfo"`
}

// CreateCursorResponse creates a response for a cursor command.
func CreateCursorResponse(cursorID int64, ns string, identifier BatchIdentifier, batch ...bson.D) bson.D {
	batchArr := bson.A{}
	for _, doc := range batch {
		batchArr = append(batchArr, doc)
	}

	return bson.D{
		{"ok", 1},
		{"cursor", bson.D{
			{"id", cursorID},
			{"ns", ns},
			{string(identifier), batchArr},
		}},
	}
}

// CreateCommandErrorResponse creates a response with a command error.
func CreateCommandErrorResponse(ce CommandError) bson.D {
	res := bson.D{
		{"ok", 0},
		{"code", ce.Code},
		{"errmsg", ce.Message},
		{"codeName", ce.Name},
	}
	if len(ce.Labels) > 0 {
		var labelsArr bson.A
		for _, label := range ce.Labels {
			labelsArr = append(labelsArr, label)
		}
		res = append(res, bson.E{Key: "labels", Value: labelsArr})
	}
	return res
}

// CreateWriteErrorsResponse creates a response with one or more write errors.
func CreateWriteErrorsResponse(writeErrorrs ...WriteError) bson.D {
	arr := make(bson.A, len(writeErrorrs))
	for idx, we := range writeErrorrs {
		arr[idx] = bson.D{
			{"index", we.Index},
			{"code", we.Code},
			{"errmsg", we.Message},
		}
	}

	return bson.D{
		{"ok", 1},
		{"writeErrors", arr},
	}
}

// CreateWriteConcernErrorResponse creates a response with a write concern error.
func CreateWriteConcernErrorResponse(wce WriteConcernError) bson.D {
	wceDoc := bson.D{
		{"code", wce.Code},
		{"codeName", wce.Name},
		{"errmsg", wce.Message},
	}
	if len(wce.Details) > 0 {
		wceDoc = append(wceDoc, bson.E{Key: "errInfo", Value: wce.Details})
	}

	return bson.D{
		{"ok", 1},
		{"writeConcernError", wceDoc},
	}
}

// CreateSuccessResponse creates a response for a successful operation with the given elements.
func CreateSuccessResponse(elems ...bson.E) bson.D {
	res := bson.D{
		{"ok", 1},
	}
	return append(res, elems...)
}
