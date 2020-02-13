// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongo

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

// NewInsertOneModel creates a new InsertOneModel.
func NewInsertOneModel() *InsertOneModel {
	return &InsertOneModel{}
}

// SetDocument sets the BSON document for the InsertOneModel.
func (iom *InsertOneModel) SetDocument(doc interface{}) *InsertOneModel {
	iom.Document = doc
	return iom
}

func (*InsertOneModel) writeModel() {}

// DeleteOneModel is the write model for delete operations.
type DeleteOneModel struct {
	Filter    interface{}
	Collation *options.Collation
}

// NewDeleteOneModel creates a new DeleteOneModel.
func NewDeleteOneModel() *DeleteOneModel {
	return &DeleteOneModel{}
}

// SetFilter sets the filter for the DeleteOneModel.
func (dom *DeleteOneModel) SetFilter(filter interface{}) *DeleteOneModel {
	dom.Filter = filter
	return dom
}

// SetCollation sets the collation for the DeleteOneModel.
func (dom *DeleteOneModel) SetCollation(collation *options.Collation) *DeleteOneModel {
	dom.Collation = collation
	return dom
}

func (*DeleteOneModel) writeModel() {}

// DeleteManyModel is the write model for deleteMany operations.
type DeleteManyModel struct {
	Filter    interface{}
	Collation *options.Collation
}

// NewDeleteManyModel creates a new DeleteManyModel.
func NewDeleteManyModel() *DeleteManyModel {
	return &DeleteManyModel{}
}

// SetFilter sets the filter for the DeleteManyModel.
func (dmm *DeleteManyModel) SetFilter(filter interface{}) *DeleteManyModel {
	dmm.Filter = filter
	return dmm
}

// SetCollation sets the collation for the DeleteManyModel.
func (dmm *DeleteManyModel) SetCollation(collation *options.Collation) *DeleteManyModel {
	dmm.Collation = collation
	return dmm
}

func (*DeleteManyModel) writeModel() {}

// ReplaceOneModel is the write model for replace operations.
type ReplaceOneModel struct {
	Collation   *options.Collation
	Upsert      *bool
	Filter      interface{}
	Replacement interface{}
}

// NewReplaceOneModel creates a new ReplaceOneModel.
func NewReplaceOneModel() *ReplaceOneModel {
	return &ReplaceOneModel{}
}

// SetFilter sets the filter for the ReplaceOneModel.
func (rom *ReplaceOneModel) SetFilter(filter interface{}) *ReplaceOneModel {
	rom.Filter = filter
	return rom
}

// SetReplacement sets the replacement document for the ReplaceOneModel.
func (rom *ReplaceOneModel) SetReplacement(rep interface{}) *ReplaceOneModel {
	rom.Replacement = rep
	return rom
}

// SetCollation sets the collation for the ReplaceOneModel.
func (rom *ReplaceOneModel) SetCollation(collation *options.Collation) *ReplaceOneModel {
	rom.Collation = collation
	return rom
}

// SetUpsert specifies if a new document should be created if no document matches the query.
func (rom *ReplaceOneModel) SetUpsert(upsert bool) *ReplaceOneModel {
	rom.Upsert = &upsert
	return rom
}

func (*ReplaceOneModel) writeModel() {}

// UpdateOneModel is the write model for update operations.
type UpdateOneModel struct {
	Collation    *options.Collation
	Upsert       *bool
	Filter       interface{}
	Update       interface{}
	ArrayFilters *options.ArrayFilters
}

// NewUpdateOneModel creates a new UpdateOneModel.
func NewUpdateOneModel() *UpdateOneModel {
	return &UpdateOneModel{}
}

// SetFilter sets the filter for the UpdateOneModel.
func (uom *UpdateOneModel) SetFilter(filter interface{}) *UpdateOneModel {
	uom.Filter = filter
	return uom
}

// SetUpdate sets the update document for the UpdateOneModel.
func (uom *UpdateOneModel) SetUpdate(update interface{}) *UpdateOneModel {
	uom.Update = update
	return uom
}

// SetArrayFilters specifies a set of filters specifying to which array elements an update should apply.
func (uom *UpdateOneModel) SetArrayFilters(filters options.ArrayFilters) *UpdateOneModel {
	uom.ArrayFilters = &filters
	return uom
}

// SetCollation sets the collation for the UpdateOneModel.
func (uom *UpdateOneModel) SetCollation(collation *options.Collation) *UpdateOneModel {
	uom.Collation = collation
	return uom
}

// SetUpsert specifies if a new document should be created if no document matches the query.
func (uom *UpdateOneModel) SetUpsert(upsert bool) *UpdateOneModel {
	uom.Upsert = &upsert
	return uom
}

func (*UpdateOneModel) writeModel() {}

// UpdateManyModel is the write model for updateMany operations.
type UpdateManyModel struct {
	Collation    *options.Collation
	Upsert       *bool
	Filter       interface{}
	Update       interface{}
	ArrayFilters *options.ArrayFilters
}

// NewUpdateManyModel creates a new UpdateManyModel.
func NewUpdateManyModel() *UpdateManyModel {
	return &UpdateManyModel{}
}

// SetFilter sets the filter for the UpdateManyModel.
func (umm *UpdateManyModel) SetFilter(filter interface{}) *UpdateManyModel {
	umm.Filter = filter
	return umm
}

// SetUpdate sets the update document for the UpdateManyModel.
func (umm *UpdateManyModel) SetUpdate(update interface{}) *UpdateManyModel {
	umm.Update = update
	return umm
}

// SetArrayFilters specifies a set of filters specifying to which array elements an update should apply.
func (umm *UpdateManyModel) SetArrayFilters(filters options.ArrayFilters) *UpdateManyModel {
	umm.ArrayFilters = &filters
	return umm
}

// SetCollation sets the collation for the UpdateManyModel.
func (umm *UpdateManyModel) SetCollation(collation *options.Collation) *UpdateManyModel {
	umm.Collation = collation
	return umm
}

// SetUpsert specifies if a new document should be created if no document matches the query.
func (umm *UpdateManyModel) SetUpsert(upsert bool) *UpdateManyModel {
	umm.Upsert = &upsert
	return umm
}

func (*UpdateManyModel) writeModel() {}
