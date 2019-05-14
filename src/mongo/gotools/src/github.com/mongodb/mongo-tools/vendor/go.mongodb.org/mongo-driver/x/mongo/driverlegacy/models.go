// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package driverlegacy

import (
	"go.mongodb.org/mongo-driver/mongo/options"
)

// WriteModel is the interface satisfied by all models for bulk writes.
type WriteModel interface {
	writeModel()
}

// InsertOneModel is the write model for insert operations.
type InsertOneModel struct {
	Document interface{}
}

func (InsertOneModel) writeModel() {}

// DeleteOneModel is the write model for delete operations.
type DeleteOneModel struct {
	Filter    interface{}
	Collation *options.Collation
}

func (DeleteOneModel) writeModel() {}

// DeleteManyModel is the write model for deleteMany operations.
type DeleteManyModel struct {
	Filter    interface{}
	Collation *options.Collation
}

func (DeleteManyModel) writeModel() {}

// UpdateModel contains the fields that are shared between the ReplaceOneModel, UpdateOneModel, and UpdateManyModel types
type UpdateModel struct {
	Collation *options.Collation
	Upsert    bool
	UpsertSet bool
}

// ReplaceOneModel is the write model for replace operations.
type ReplaceOneModel struct {
	Filter      interface{}
	Replacement interface{}
	UpdateModel
}

func (ReplaceOneModel) writeModel() {}

// UpdateOneModel is the write model for update operations.
type UpdateOneModel struct {
	Filter interface{}
	Update interface{}
	// default is to not send a value. for servers < 3.6, error raised if value given. for unack writes using opcodes,
	// error raised if value given
	ArrayFilters    options.ArrayFilters
	ArrayFiltersSet bool
	UpdateModel
}

func (UpdateOneModel) writeModel() {}

// UpdateManyModel is the write model for updateMany operations.
type UpdateManyModel struct {
	Filter interface{}
	Update interface{}
	// default is to not send a value. for servers < 3.6, error raised if value given. for unack writes using opcodes,
	// error raised if value given
	ArrayFilters    options.ArrayFilters
	ArrayFiltersSet bool
	UpdateModel
}

func (UpdateManyModel) writeModel() {}
