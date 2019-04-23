// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Package auth provides utilities for performing tasks related to authentication.
package auth

import (
	"fmt"
	"strings"

	"github.com/mongodb/mongo-tools-common/db"
	"github.com/mongodb/mongo-tools-common/util"
	"go.mongodb.org/mongo-driver/bson"
)

// GetAuthVersion gets the authentication schema version of the connected server
// and returns that value as an integer along with any error that occurred.
func GetAuthVersion(sessionProvider *db.SessionProvider) (int, error) {
	results := bson.M{}
	err := sessionProvider.Run(
		bson.D{
			{"getParameter", 1},
			{"authSchemaVersion", 1},
		},
		&results,
		"admin",
	)

	if err != nil {
		errMessage := err.Error()
		// as a necessary hack, if the error message takes a certain form,
		// we can infer version 1. This is because early versions of mongodb
		// had no concept of an "auth schema version", so asking for the
		// authSchemaVersion value will return a "no option found" or "no such cmd"
		if errMessage == "no option found to get" ||
			strings.Contains(errMessage, "no such cmd") {
			return 1, nil
		}
		// otherwise it's a connection error, so bubble it up
		return 0, err
	}

	version, err := util.ToInt(results["authSchemaVersion"])
	if err != nil {
		return 0, fmt.Errorf("getParameter command returned non-numeric result: %v, error: %v", results["authSchemaVersion"], err)
	}
	return version, nil
}

// VerifySystemAuthVersion returns an error if authentication is not set up for
// the given server.
func VerifySystemAuthVersion(sessionProvider *db.SessionProvider) error {
	session, err := sessionProvider.GetSession()
	if err != nil {
		return fmt.Errorf("error getting session from server: %v", err)
	}

	authSchemaQuery := bson.M{"_id": "authSchema"}
	count, err := session.Database("admin").Collection("system.version").CountDocuments(nil, authSchemaQuery)
	if err != nil {
		return fmt.Errorf("error checking pressence of auth version: %v", err)
	} else if count == 0 {
		return fmt.Errorf("found no auth version")
	}
	return nil
}
