// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package db

import (
	"fmt"
	"time"

	"github.com/mongodb/mongo-tools-common/json"
	"go.mongodb.org/mongo-driver/mongo/readpref"
	"go.mongodb.org/mongo-driver/tag"
	"go.mongodb.org/mongo-driver/x/network/connstring"
)

// readPrefDoc is a struct corresponding to the json object passed in for the --readPreference command line arg.
type readPrefDoc struct {
	Mode                *string
	TagSets             []map[string]string
	MaxStalenessSeconds *int
}

const (
	WarningNonPrimaryMongosConnection = "Warning: using a non-primary readPreference with a " +
		"connection to mongos may produce inconsistent duplicates or miss some documents."
)

// NewReadPreference takes a string (command line read preference argument) and a ConnString (from the command line
// URI argument) and returns a ReadPref. If both are provided, preference is given to the command line argument. If
// both are empty, a default read preference of primary will be returned.
func NewReadPreference(rp string, cs *connstring.ConnString) (*readpref.ReadPref, error) {
	if rp == "" && (cs == nil || cs.ReadPreference == "") {
		return readpref.Primary(), nil
	}

	if rp == "" {
		return readPrefFromConnString(cs)
	}

	var mode string
	var options []readpref.Option
	if rp[0] != '{' {
		mode = rp
	} else {
		var doc readPrefDoc
		err := json.Unmarshal([]byte(rp), &doc)
		if err != nil {
			return nil, fmt.Errorf("invalid json object: %v", err)
		}

		if doc.Mode == nil {
			return nil, fmt.Errorf("no 'mode' specified")
		}
		mode = *doc.Mode

		if doc.TagSets != nil {
			options = append(options, readpref.WithTagSets(tag.NewTagSetsFromMaps(doc.TagSets)...))
		}

		if doc.MaxStalenessSeconds != nil {
			options = append(options, readpref.WithMaxStaleness(time.Duration(*doc.MaxStalenessSeconds)*time.Second))
		}
	}

	rpMode, err := readpref.ModeFromString(mode)
	if err != nil {
		return nil, err
	}

	return readpref.New(rpMode, options...)
}

func readPrefFromConnString(cs *connstring.ConnString) (*readpref.ReadPref, error) {
	var opts []readpref.Option

	tagSets := tag.NewTagSetsFromMaps(cs.ReadPreferenceTagSets)
	if len(tagSets) > 0 {
		opts = append(opts, readpref.WithTagSets(tagSets...))
	}

	if cs.MaxStalenessSet {
		opts = append(opts, readpref.WithMaxStaleness(cs.MaxStaleness))
	}

	mode, err := readpref.ModeFromString(cs.ReadPreference)
	if err != nil {
		return nil, err
	}

	return readpref.New(mode, opts...)
}
