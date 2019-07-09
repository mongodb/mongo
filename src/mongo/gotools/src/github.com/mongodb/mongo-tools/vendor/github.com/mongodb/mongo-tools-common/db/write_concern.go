// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package db

import (
	"github.com/mongodb/mongo-tools-common/json"
	"github.com/mongodb/mongo-tools-common/log"
	"github.com/mongodb/mongo-tools-common/util"
	"go.mongodb.org/mongo-driver/mongo/writeconcern"
	"go.mongodb.org/mongo-driver/x/mongo/driver/connstring"

	"fmt"
	"strconv"
	"time"
)

// write concern fields
const (
	j         = "j"
	w         = "w"
	wTimeout  = "wtimeout"
	majString = "majority"
)

// NewMongoWriteConcern takes a string (from the command line writeConcern option) and a ConnString object
// (from the command line uri option) and returns a WriteConcern. If both are provided, preference is given to
// the command line writeConcern option. If neither is provided, the default 'majority' write concern is constructed.
func NewMongoWriteConcern(writeConcern string, cs *connstring.ConnString) (wc *writeconcern.WriteConcern, err error) {

	// Log whatever write concern was generated
	defer func() {
		if wc != nil {
			log.Logvf(log.Info, "using write concern: %v", wc)
		}
	}()

	// URI Connection String provided but no String provided case; constructWCFromConnString handles
	// default for ConnString without write concern
	if writeConcern == "" && cs != nil {
		return constructWCFromConnString(cs)
	}

	// String case; constructWCFromString handles default for empty string
	return constructWCFromString(writeConcern)
}

// constructWCFromConnString takes in a parsed connection string and
// extracts values from it. If the ConnString has no write concern value, it defaults
// to 'majority'.
func constructWCFromConnString(cs *connstring.ConnString) (*writeconcern.WriteConcern, error) {
	var opts []writeconcern.Option

	switch {
	case cs.WNumberSet:
		if cs.WNumber < 0 {
			return nil, fmt.Errorf("invalid 'w' argument: %v", cs.WNumber)
		}

		opts = append(opts, writeconcern.W(cs.WNumber))
	case cs.WString != "":
		opts = append(opts, writeconcern.WTagSet(cs.WString))
	default:
		opts = append(opts, writeconcern.WMajority())
	}

	opts = append(opts, writeconcern.J(cs.J))
	opts = append(opts, writeconcern.WTimeout(cs.WTimeout))

	return writeconcern.New(opts...), nil
}

// constructWCFromString takes in a write concern and attempts to
// extract values from it. It returns an error if it is unable to parse the
// string or if a parsed write concern field value is invalid.
func constructWCFromString(writeConcern string) (*writeconcern.WriteConcern, error) {

	// Default case
	if writeConcern == "" {
		return writeconcern.New(writeconcern.WMajority()), nil
	}

	// Try to unmarshal as JSON document
	jsonWriteConcern := map[string]interface{}{}
	err := json.Unmarshal([]byte(writeConcern), &jsonWriteConcern)
	if err == nil {
		return parseJSONWriteConcern(jsonWriteConcern)
	}

	// If JSON parsing fails, try to parse it as a plain string instead.  This
	// allows a default to the old behavior wherein the entire argument passed
	// in is assigned to the 'w' field - thus allowing users pass a write
	// concern that looks like: "majority", 0, "4", etc.
	wOpt, err := parseModeString(writeConcern)
	if err != nil {
		return nil, err
	}

	return writeconcern.New(wOpt), err
}

// parseJSONWriteConcern converts a JSON map representing a write concern object into a WriteConcern
func parseJSONWriteConcern(jsonWriteConcern map[string]interface{}) (*writeconcern.WriteConcern, error) {
	var opts []writeconcern.Option

	// Construct new options from 'w', if it exists; otherwise default to 'majority'
	if wVal, ok := jsonWriteConcern[w]; ok {
		opt, err := parseWField(wVal)
		if err != nil {
			return nil, err
		}

		opts = append(opts, opt)
	} else {
		opts = append(opts, writeconcern.WMajority())
	}

	// Journal option
	if jVal, ok := jsonWriteConcern[j]; ok && util.IsTruthy(jVal) {
		opts = append(opts, writeconcern.J(true))
	}

	// Wtimeout option
	if wtimeout, ok := jsonWriteConcern[wTimeout]; ok {
		timeoutVal, err := util.ToInt(wtimeout)
		if err != nil {
			return nil, fmt.Errorf("invalid '%v' argument: %v", wTimeout, wtimeout)
		}
		// Previous implementation assumed passed in string was milliseconds
		opts = append(opts, writeconcern.WTimeout(time.Duration(timeoutVal)*time.Millisecond))
	}

	return writeconcern.New(opts...), nil
}

func parseWField(wValue interface{}) (writeconcern.Option, error) {
	// Try parsing as int
	if wNumber, err := util.ToInt(wValue); err == nil {
		return parseModeNumber(wNumber)
	}

	// Try parsing as string
	if wStrVal, ok := wValue.(string); ok {
		return parseModeString(wStrVal)
	}

	return nil, fmt.Errorf("invalid 'w' argument type: %v has type %T", wValue, wValue)
}

// Given an integer, returns a write concern object or error
func parseModeNumber(wNumber int) (writeconcern.Option, error) {
	if wNumber < 0 {
		return nil, fmt.Errorf("invalid 'w' argument: %v", wNumber)
	}

	return writeconcern.W(wNumber), nil
}

// Given a string, returns a write concern object or error
func parseModeString(wString string) (writeconcern.Option, error) {
	// Default case
	if wString == "" {
		return writeconcern.WMajority(), nil
	}

	// Try parsing as number before treating as just a string
	if wNumber, err := strconv.Atoi(wString); err == nil {
		return parseModeNumber(wNumber)
	}

	return writeconcern.WTagSet(wString), nil
}
