// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package command

import (
	"errors"
	"strings"
)

// Namespace encapsulates a database and collection name, which together
// uniquely identifies a collection within a MongoDB cluster.
type Namespace struct {
	DB         string
	Collection string
}

// NewNamespace returns a new Namespace for the
// given database and collection.
func NewNamespace(db, collection string) Namespace { return Namespace{DB: db, Collection: collection} }

// ParseNamespace parses a namespace string into a Namespace.
//
// The namespace string must contain at least one ".", the first of which is the separator
// between the database and collection names.  If not, the default (invalid) Namespace is returned.
func ParseNamespace(name string) Namespace {
	index := strings.Index(name, ".")
	if index == -1 {
		return Namespace{}
	}

	return Namespace{
		DB:         name[:index],
		Collection: name[index+1:],
	}
}

// FullName returns the full namespace string, which is the result of joining the database
// name and the collection name with a "." character.
func (ns *Namespace) FullName() string {
	return strings.Join([]string{ns.DB, ns.Collection}, ".")
}

// Validate validates the namespace.
func (ns *Namespace) Validate() error {
	if err := ns.validateDB(); err != nil {
		return err
	}

	return ns.validateCollection()
}

// validateDB ensures the database name is not an empty string, contain a ".",
// or contain a " ".
func (ns *Namespace) validateDB() error {
	if ns.DB == "" {
		return errors.New("database name cannot be empty")
	}
	if strings.Contains(ns.DB, " ") {
		return errors.New("database name cannot contain ' '")
	}
	if strings.Contains(ns.DB, ".") {
		return errors.New("database name cannot contain '.'")
	}

	return nil
}

// validateCollection ensures the collection name is not an empty string.
func (ns *Namespace) validateCollection() error {
	if ns.Collection == "" {
		return errors.New("collection name cannot be empty")
	}

	return nil
}
