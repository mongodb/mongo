package util

import (
	"fmt"
	"strings"
)

// Split the host string into the individual nodes to connect to, appending the
// port if necessary.
func CreateConnectionAddrs(host, port string) []string {

	// parse the host string into the individual hosts
	addrs := parseHost(host)

	// if a port is specified, append it to all the hosts
	if port != "" {
		for idx, addr := range addrs {
			addrs[idx] = fmt.Sprintf("%v:%v", addr, port)
		}
	}

	return addrs
}

// Helper function for parsing the host string into addresses.  Returns a slice
// of the individual addresses to use to connect.
func parseHost(host string) []string {

	// strip off the replica set name from the beginning
	slashIndex := strings.Index(host, "/")
	if slashIndex != -1 {
		if slashIndex == len(host)-1 {
			return []string{""}
		}
		host = host[slashIndex+1:]
	}

	// split into the individual hosts
	return strings.Split(host, ",")
}

// SplitNamespace splits a namespace path into a database and collection,
// returned in that order. An error is returned if the namespace is invalid.
func SplitAndValidateNamespace(namespace string) (string, string, error) {

	// first, run validation checks
	if err := ValidateFullNamespace(namespace); err != nil {
		return "", "", fmt.Errorf("namespace '%v' is not valid: %v",
			namespace, err)
	}

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
		return fmt.Errorf("'%v' is longer than 63 characters", database)
	}

	// check for illegal characters
	if strings.ContainsAny(database, "/\\. \""+string([]byte{0})) {
		return fmt.Errorf("illegal character found in '%v'", database)
	}

	// db name is valid
	return nil
}

// ValidateCollectionName validates that a string is a valid name for a mongodb
// collection. An error is returned if it is not valid.
func ValidateCollectionName(collection string) error {

	// collection names cannot begin with 'system.'
	if strings.HasPrefix(collection, "system.") {
		return fmt.Errorf("'%v' is not allowed to begin with 'system.'",
			collection)
	}

	// check for illegal characters
	if strings.ContainsAny(collection, "$"+string([]byte{0})) {
		return fmt.Errorf("illegal character found in '%v'", collection)
	}

	// collection name is valid
	return nil
}
