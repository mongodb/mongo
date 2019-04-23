// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package command

import (
	"errors"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/x/bsonx/bsoncore"
	"go.mongodb.org/mongo-driver/x/network/result"
)

// unmarshalFindAndModifyResult turns the provided bson.Reader into a findAndModify result.
func unmarshalFindAndModifyResult(rdr bson.Raw) (result.FindAndModify, error) {
	var res result.FindAndModify

	val, err := rdr.LookupErr("value")
	switch {
	case err == bsoncore.ErrElementNotFound:
		return result.FindAndModify{}, errors.New("invalid response from server, no value field")
	case err != nil:
		return result.FindAndModify{}, err
	}

	switch val.Type {
	case bson.TypeNull:
	case bson.TypeEmbeddedDocument:
		res.Value = val.Document()
	default:
		return result.FindAndModify{}, errors.New("invalid response from server, 'value' field is not a document")
	}

	if val, err := rdr.LookupErr("lastErrorObject", "updatedExisting"); err == nil {
		b, ok := val.BooleanOK()
		if ok {
			res.LastErrorObject.UpdatedExisting = b
		}
	}

	if val, err := rdr.LookupErr("lastErrorObject", "upserted"); err == nil {
		oid, ok := val.ObjectIDOK()
		if ok {
			res.LastErrorObject.Upserted = oid
		}
	}
	if val, err := rdr.LookupErr("writeConcernError"); err == nil {
		_ = val.Unmarshal(&res.WriteConcernError)
	}
	return res, nil
}
