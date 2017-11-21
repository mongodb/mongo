// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package db

import (
	"fmt"

	"github.com/mongodb/mongo-tools/common/json"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
)

type readPrefDoc struct {
	Mode string
	Tags bson.D
}

const (
	WarningNonPrimaryMongosConnection = "Warning: using a non-primary readPreference with a " +
		"connection to mongos may produce inconsistent duplicates or miss some documents."
)

func ParseReadPreference(rp string) (mgo.Mode, bson.D, error) {
	var mode string
	var tags bson.D
	if rp == "" {
		return mgo.Nearest, nil, nil
	}
	if rp[0] != '{' {
		mode = rp
	} else {
		var doc readPrefDoc
		err := json.Unmarshal([]byte(rp), &doc)
		if err != nil {
			return 0, nil, fmt.Errorf("invalid --ReadPreferences json object: %v", err)
		}
		tags = doc.Tags
		mode = doc.Mode
	}
	switch mode {
	case "primary":
		return mgo.Primary, tags, nil
	case "primaryPreferred":
		return mgo.PrimaryPreferred, tags, nil
	case "secondary":
		return mgo.Secondary, tags, nil
	case "secondaryPreferred":
		return mgo.SecondaryPreferred, tags, nil
	case "nearest":
		return mgo.Nearest, tags, nil
	}
	return 0, nil, fmt.Errorf("invalid readPreference mode '%v'", mode)
}
