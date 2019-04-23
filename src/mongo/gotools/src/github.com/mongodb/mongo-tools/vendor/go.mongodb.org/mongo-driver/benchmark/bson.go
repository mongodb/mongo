// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package benchmark

import (
	"errors"
	"io/ioutil"
	"path/filepath"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/x/bsonx"
)

const (
	perfDataDir  = "perf"
	bsonDataDir  = "extended_bson"
	flatBSONData = "flat_bson.json"
	deepBSONData = "deep_bson.json"
	fullBSONData = "full_bson.json"
)

// utility functions for the bson benchmarks

func loadSourceDocument(pathParts ...string) (bsonx.Doc, error) {
	data, err := ioutil.ReadFile(filepath.Join(pathParts...))
	if err != nil {
		return nil, err
	}
	doc := bsonx.Doc{}
	err = bson.UnmarshalExtJSON(data, true, &doc)
	if err != nil {
		return nil, err
	}

	if len(doc) == 0 {
		return nil, errors.New("empty bson document")
	}

	return doc, nil
}

func loadSourceRaw(pathParts ...string) (bson.Raw, error) {
	doc, err := loadSourceDocument(pathParts...)
	if err != nil {
		return nil, err
	}
	raw, err := doc.MarshalBSON()
	if err != nil {
		return nil, err
	}

	return bson.Raw(raw), nil
}

func loadSourceD(pathParts ...string) (bson.D, error) {
	data, err := ioutil.ReadFile(filepath.Join(pathParts...))
	if err != nil {
		return nil, err
	}
	doc := bson.D{}
	err = bson.UnmarshalExtJSON(data, true, &doc)
	if err != nil {
		return nil, err
	}

	if len(doc) == 0 {
		return nil, errors.New("empty bson document")
	}

	return doc, nil
}
