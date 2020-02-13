// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongo

import (
	"go.mongodb.org/mongo-driver/bson"
)

// IndexOptionsBuilder constructs a BSON document for index options
type IndexOptionsBuilder struct {
	document bson.D
}

// NewIndexOptionsBuilder creates a new instance of IndexOptionsBuilder
func NewIndexOptionsBuilder() *IndexOptionsBuilder {
	return &IndexOptionsBuilder{}
}

// Background sets the background option
func (iob *IndexOptionsBuilder) Background(background bool) *IndexOptionsBuilder {
	iob.document = append(iob.document, bson.E{"background", background})
	return iob
}

// ExpireAfterSeconds sets the expireAfterSeconds option
func (iob *IndexOptionsBuilder) ExpireAfterSeconds(expireAfterSeconds int32) *IndexOptionsBuilder {
	iob.document = append(iob.document, bson.E{"expireAfterSeconds", expireAfterSeconds})
	return iob
}

// Name sets the name option
func (iob *IndexOptionsBuilder) Name(name string) *IndexOptionsBuilder {
	iob.document = append(iob.document, bson.E{"name", name})
	return iob
}

// Sparse sets the sparse option
func (iob *IndexOptionsBuilder) Sparse(sparse bool) *IndexOptionsBuilder {
	iob.document = append(iob.document, bson.E{"sparse", sparse})
	return iob
}

// StorageEngine sets the storageEngine option
func (iob *IndexOptionsBuilder) StorageEngine(storageEngine interface{}) *IndexOptionsBuilder {
	iob.document = append(iob.document, bson.E{"storageEngine", storageEngine})
	return iob
}

// Unique sets the unique option
func (iob *IndexOptionsBuilder) Unique(unique bool) *IndexOptionsBuilder {
	iob.document = append(iob.document, bson.E{"unique", unique})
	return iob
}

// Version sets the version option
func (iob *IndexOptionsBuilder) Version(version int32) *IndexOptionsBuilder {
	iob.document = append(iob.document, bson.E{"v", version})
	return iob
}

// DefaultLanguage sets the defaultLanguage option
func (iob *IndexOptionsBuilder) DefaultLanguage(defaultLanguage string) *IndexOptionsBuilder {
	iob.document = append(iob.document, bson.E{"default_language", defaultLanguage})
	return iob
}

// LanguageOverride sets the languageOverride option
func (iob *IndexOptionsBuilder) LanguageOverride(languageOverride string) *IndexOptionsBuilder {
	iob.document = append(iob.document, bson.E{"language_override", languageOverride})
	return iob
}

// TextVersion sets the textVersion option
func (iob *IndexOptionsBuilder) TextVersion(textVersion int32) *IndexOptionsBuilder {
	iob.document = append(iob.document, bson.E{"textIndexVersion", textVersion})
	return iob
}

// Weights sets the weights option
func (iob *IndexOptionsBuilder) Weights(weights interface{}) *IndexOptionsBuilder {
	iob.document = append(iob.document, bson.E{"weights", weights})
	return iob
}

// SphereVersion sets the sphereVersion option
func (iob *IndexOptionsBuilder) SphereVersion(sphereVersion int32) *IndexOptionsBuilder {
	iob.document = append(iob.document, bson.E{"2dsphereIndexVersion", sphereVersion})
	return iob
}

// Bits sets the bits option
func (iob *IndexOptionsBuilder) Bits(bits int32) *IndexOptionsBuilder {
	iob.document = append(iob.document, bson.E{"bits", bits})
	return iob
}

// Max sets the max option
func (iob *IndexOptionsBuilder) Max(max float64) *IndexOptionsBuilder {
	iob.document = append(iob.document, bson.E{"max", max})
	return iob
}

// Min sets the min option
func (iob *IndexOptionsBuilder) Min(min float64) *IndexOptionsBuilder {
	iob.document = append(iob.document, bson.E{"min", min})
	return iob
}

// BucketSize sets the bucketSize option
func (iob *IndexOptionsBuilder) BucketSize(bucketSize int32) *IndexOptionsBuilder {
	iob.document = append(iob.document, bson.E{"bucketSize", bucketSize})
	return iob
}

// PartialFilterExpression sets the partialFilterExpression option
func (iob *IndexOptionsBuilder) PartialFilterExpression(partialFilterExpression interface{}) *IndexOptionsBuilder {
	iob.document = append(iob.document, bson.E{"partialFilterExpression", partialFilterExpression})
	return iob
}

// Collation sets the collation option
func (iob *IndexOptionsBuilder) Collation(collation interface{}) *IndexOptionsBuilder {
	iob.document = append(iob.document, bson.E{"collation", collation})
	return iob
}

// Build returns the BSON document from the builder
func (iob *IndexOptionsBuilder) Build() bson.D {
	return iob.document
}
