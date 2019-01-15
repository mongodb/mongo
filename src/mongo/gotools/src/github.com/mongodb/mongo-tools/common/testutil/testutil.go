// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Package testutil implements functions for filtering and configuring tests.
package testutil

import (
	"os"
	"strconv"
	"strings"

	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/options"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
)

// GetBareSession returns an mgo.Session from the environment or
// from a default host and port.
func GetBareSession() (*mgo.Session, error) {
	sessionProvider, _, err := GetBareSessionProvider()
	if err != nil {
		return nil, err
	}
	session, err := sessionProvider.GetSession()
	if err != nil {
		return nil, err
	}
	return session, nil
}

// GetBareSessionProvider returns a session provider from the environment or
// from a default host and port.
func GetBareSessionProvider() (*db.SessionProvider, *options.ToolOptions, error) {
	var toolOptions *options.ToolOptions

	// get ToolOptions from URI or defaults
	if uri := os.Getenv("TOOLS_TESTING_MONGOD"); uri != "" {
		fakeArgs := []string{"--uri=" + uri}
		toolOptions = options.New("mongodump", "", options.EnabledOptions{URI: true})
		toolOptions.URI.AddKnownURIParameters(options.KnownURIOptionsReadPreference)
		_, err := toolOptions.ParseArgs(fakeArgs)
		if err != nil {
			panic("Could not parse MONGOD environment variable")
		}
	} else {
		ssl := GetSSLOptions()
		auth := GetAuthOptions()
		connection := &options.Connection{
			Host: "localhost",
			Port: db.DefaultTestPort,
		}
		toolOptions = &options.ToolOptions{
			SSL:        &ssl,
			Connection: connection,
			Auth:       &auth,
			Verbosity:  &options.Verbosity{},
			URI:        &options.URI{},
		}
	}
	sessionProvider, err := db.NewSessionProvider(*toolOptions)
	if err != nil {
		return nil, nil, err
	}
	return sessionProvider, toolOptions, nil
}

// GetFCV returns the featureCompatibilityVersion string for an mgo Session
// or the empty string if it can't be found.
func GetFCV(s *mgo.Session) string {
	coll := s.DB("admin").C("system.version")
	var result struct {
		Version string
	}
	_ = coll.Find(bson.M{"_id": "featureCompatibilityVersion"}).One(&result)
	return result.Version
}

// CompareFCV compares two strings as dot-delimited tuples of integers
func CompareFCV(x, y string) (int, error) {
	left, err := dottedStringToSlice(x)
	if err != nil {
		return 0, err
	}
	right, err := dottedStringToSlice(y)
	if err != nil {
		return 0, err
	}

	// Ensure left is the shorter one, flip logic if necessary
	inverter := 1
	if len(right) < len(left) {
		inverter = -1
		left, right = right, left
	}

	for i := range left {
		switch {
		case left[i] < right[i]:
			return -1 * inverter, nil
		case left[i] > right[i]:
			return 1 * inverter, nil
		}
	}

	// compared equal to length of left. If right is longer, then left is less
	// than right (-1) (modulo the inverter)
	if len(left) < len(right) {
		return -1 * inverter, nil
	}

	return 0, nil
}

func dottedStringToSlice(s string) ([]int, error) {
	parts := make([]int, 0, 2)
	for _, v := range strings.Split(s, ".") {
		i, err := strconv.Atoi(v)
		if err != nil {
			return parts, err
		}
		parts = append(parts, i)
	}
	return parts, nil
}

func IsReplicaSet(s *mgo.Session) bool {
	db := s.DB("admin")
	var result bson.M
	cmd := bson.M{"isMaster": 1}
	db.Run(cmd, &result)
	_, ok := result["setName"]
	return ok
}
