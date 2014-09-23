package auth

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2/bson"
	"strings"
)

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
		return 0, err
	}

	if util.IsTruthy(results["ok"]) {
		version, ok := results["authSchemaVersion"].(int)
		if !ok {
			// very unlikely this will ever happen
			return 0, fmt.Errorf(
				"getParameter command returned non-numeric result: %v",
				results["authSchemaVersion"])
		}
		return version, nil
	}
	errMessage, ok := results["errmsg"].(string)
	if !ok {
		// errmsg will always be returned on error unless the command API changes
		return 0, fmt.Errorf(
			"getParameter command returned non-string errmsg: %v",
			results["authSchemaVersion"])
	}
	// as a necessary hack, if the error message takes a certain form,
	// we can infer version 1. This is because early versions of mongodb
	// had no concept of an "auth schema version", so asking for the
	// authSchemaVersion value will return a "no option found" or "no such cmd"
	if errMessage == "no option found to get" ||
		strings.HasPrefix(errMessage, "no such cmd") {
		return 1, nil
	}
	return 0, fmt.Errorf(errMessage)
}
