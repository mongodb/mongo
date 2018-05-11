// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package util

import (
	"fmt"
	"strings"
)

const (
	InvalidDBChars         = "/\\. \"\x00$"
	InvalidCollectionChars = "$\x00"
	DefaultHost            = "localhost"
	DefaultPort            = "27017"
)

// Extract the replica set name and the list of hosts from the connection string
func ParseConnectionString(connString string) ([]string, string) {

	// strip off the replica set name from the beginning
	slashIndex := strings.Index(connString, "/")
	setName := ""
	if slashIndex != -1 {
		setName = connString[:slashIndex]
		if slashIndex == len(connString)-1 {
			return []string{""}, setName
		}
		connString = connString[slashIndex+1:]
	}

	// split the hosts, and return them and the set name
	return strings.Split(connString, ","), setName
}

// Split the host string into the individual nodes to connect to, appending the
// port if necessary.
func CreateConnectionAddrs(host, port string) []string {

	// set to the defaults, if necessary
	if host == "" {
		host = DefaultHost
		if port == "" {
			host += fmt.Sprintf(":%v", DefaultPort)
		}
	}

	// parse the host string into the individual hosts
	addrs, _ := ParseConnectionString(host)

	// if a port is specified, append it to all the hosts
	if port != "" {
		for idx, addr := range addrs {
			addrs[idx] = fmt.Sprintf("%v:%v", addr, port)
		}
	}

	return addrs
}

// SplitNamespace splits a namespace path into a database and collection,
// returned in that order.
func SplitNamespace(namespace string) (string, string) {

	// find the first instance of "." in the namespace
	firstDotIndex := strings.Index(namespace, ".")

	// split the namespace, if applicable
	var database string
	var collection string
	if firstDotIndex != -1 {
		database = namespace[:firstDotIndex]
		collection = namespace[firstDotIndex+1:]
	} else {
		database = namespace
	}

	return database, collection
}

// SplitAndValidateNamespace splits a namespace path into a database and collection,
// returned in that order. An error is returned if the namespace is invalid.
func SplitAndValidateNamespace(namespace string) (string, string, error) {

	// first, run validation checks
	if err := ValidateFullNamespace(namespace); err != nil {
		return "", "", fmt.Errorf("namespace '%v' is not valid: %v",
			namespace, err)
	}

	database, collection := SplitNamespace(namespace)
	return database, collection, nil
}

// ValidateFullNamespace validates a full mongodb namespace (database +
// collection), returning an error if it is invalid.
func ValidateFullNamespace(namespace string) error {

	// the namespace must be shorter than 123 bytes
	if len([]byte(namespace)) > 122 {
		return fmt.Errorf("namespace %v is too long (>= 123 bytes)", namespace)
	}

	// find the first instance of "." in the namespace
	firstDotIndex := strings.Index(namespace, ".")

	// the namespace cannot begin with a dot
	if firstDotIndex == 0 {
		return fmt.Errorf("namespace %v begins with a '.'", namespace)
	}

	// the namespace cannot end with a dot
	if firstDotIndex == len(namespace)-1 {
		return fmt.Errorf("namespace %v ends with a '.'", namespace)
	}

	// split the namespace, if applicable
	var database string
	var collection string
	if firstDotIndex != -1 {
		database = namespace[:firstDotIndex]
		collection = namespace[firstDotIndex+1:]
	} else {
		database = namespace
	}

	// validate the database name
	dbValidationErr := ValidateDBName(database)
	if dbValidationErr != nil {
		return fmt.Errorf("database name is invalid: %v", dbValidationErr)
	}

	// validate the collection name, if necessary
	if collection != "" {
		collValidationErr := ValidateCollectionName(collection)
		if collValidationErr != nil {
			return fmt.Errorf("collection name is invalid: %v",
				collValidationErr)
		}
	}

	// the namespace is valid
	return nil

}

// ValidateDBName validates that a string is a valid name for a mongodb
// database. An error is returned if it is not valid.
func ValidateDBName(database string) error {

	// must be < 64 characters
	if len([]byte(database)) > 63 {
		return fmt.Errorf("db name '%v' is longer than 63 characters", database)
	}

	// check for illegal characters
	for _, illegalRune := range InvalidDBChars {
		if strings.ContainsRune(database, illegalRune) {
			return fmt.Errorf("illegal character '%c' found in db name '%v'", illegalRune, database)
		}
	}

	// db name is valid
	return nil
}

// ValidateCollectionName validates that a string is a valid name for a mongodb
// collection. An error is returned if it is not valid.
func ValidateCollectionName(collection string) error {
	// collection names cannot begin with 'system.'
	if strings.HasPrefix(collection, "system.") {
		return fmt.Errorf("collection name '%v' is not allowed to begin with"+
			" 'system.'", collection)
	}

	return ValidateCollectionGrammar(collection)
}

// ValidateCollectionGrammar validates the collection for character and length
// errors without erroring on system collections. For validation of functionality
// that manipulates system collections.
func ValidateCollectionGrammar(collection string) error {

	// collection names cannot be empty
	if len(collection) == 0 {
		return fmt.Errorf("collection name cannot be an empty string")
	}

	// check for illegal characters
	for _, illegalRune := range InvalidCollectionChars {
		if strings.ContainsRune(collection, illegalRune) {
			return fmt.Errorf("illegal character '%c' found in '%v'", illegalRune, collection)
		}
	}

	// collection name is valid
	return nil
}
