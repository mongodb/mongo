// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package command

import (
	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

func decodeCommandOpMsg(msg wiremessage.Msg) (bson.Raw, error) {
	var mainDoc bsonx.Doc

	for _, section := range msg.Sections {
		switch converted := section.(type) {
		case wiremessage.SectionBody:
			err := mainDoc.UnmarshalBSON(converted.Document)
			if err != nil {
				return nil, err
			}
		case wiremessage.SectionDocumentSequence:
			arr := bsonx.Arr{}
			for _, doc := range converted.Documents {
				newDoc := bsonx.Doc{}
				err := newDoc.UnmarshalBSON(doc)
				if err != nil {
					return nil, err
				}

				arr = append(arr, bsonx.Document(newDoc))
			}

			mainDoc = append(mainDoc, bsonx.Elem{converted.Identifier, bsonx.Array(arr)})
		}
	}

	byteArray, err := mainDoc.MarshalBSON()
	if err != nil {
		return nil, err
	}

	rdr := bson.Raw(byteArray)
	err = rdr.Validate()
	if err != nil {
		return nil, NewCommandResponseError("malformed OP_MSG: invalid document", err)
	}

	err = extractError(rdr)
	if err != nil {
		return nil, err
	}
	return rdr, nil
}
