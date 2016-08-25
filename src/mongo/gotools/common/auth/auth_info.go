// Package auth provides utilities for performing tasks related to authentication.
package auth

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"gopkg.in/mgo.v2/bson"
	"strings"
)

// GetAuthVersion gets the authentication schema version of the connected server
// and returns that value as an integer along with any error that occurred.
func GetAuthVersion(commander db.CommandRunner) (int, error) {
	results := bson.M{}
	err := commander.Run(
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
			strings.HasPrefix(errMessage, "no such cmd") {
			return 1, nil
		}
		// otherwise it's a connection error, so bubble it up
		return 0, err
	}

	version, ok := results["authSchemaVersion"].(int)
	if !ok {
		// very unlikely this will ever happen
		return 0, fmt.Errorf(
			"getParameter command returned non-numeric result: %v",
			results["authSchemaVersion"])
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
	defer session.Close()
	versionEntries := session.DB("admin").C("system.version")
	if count, err := versionEntries.Count(); err != nil {
		return fmt.Errorf("error checking pressence of auth version: %v", err)
	} else if count == 0 {
		return fmt.Errorf("found no auth version")
	}
	return nil
}
