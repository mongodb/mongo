// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mgocompat

import (
	"reflect"
	"time"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/bsoncodec"
	"go.mongodb.org/mongo-driver/bson/bsonoptions"
	"go.mongodb.org/mongo-driver/bson/bsontype"
)

var (
	tInt            = reflect.TypeOf(int(0))
	tUint           = reflect.TypeOf(uint(0))
	tTime           = reflect.TypeOf(time.Time{})
	tM              = reflect.TypeOf(bson.M{})
	tInterfaceSlice = reflect.TypeOf([]interface{}{})
	tByteSlice      = reflect.TypeOf([]byte{})
)

// mgoRegistry is the default bsoncodec.Registry. It contains the default codecs and the
// primitive codecs.
var mgoRegistry = newRegistryBuilder().Build()

// newRegistryBuilder creates a new RegistryBuilder configured with the default encoders and
// deocders from the bsoncodec.DefaultValueEncoders and bsoncodec.DefaultValueDecoders types and the
// PrimitiveCodecs type in this package.
func newRegistryBuilder() *bsoncodec.RegistryBuilder {
	rb := bsoncodec.NewRegistryBuilder()
	bsoncodec.DefaultValueEncoders{}.RegisterDefaultEncoders(rb)
	bsoncodec.DefaultValueDecoders{}.RegisterDefaultDecoders(rb)
	bson.PrimitiveCodecs{}.RegisterPrimitiveCodecs(rb)

	structcodec, _ := bsoncodec.NewStructCodec(bsoncodec.DefaultStructTagParser,
		bsonoptions.StructCodec().
			SetDecodeZeroStruct(true).
			SetDecodeDeepZeroInline(true).
			SetEncodeOmitDefaultStruct(true).
			SetAllowUnexportedFields(true))

	rb.RegisterDefaultDecoder(reflect.String, bsoncodec.NewStringCodec(bsonoptions.StringCodec().SetDecodeObjectIDAsHex(false))).
		RegisterDefaultDecoder(reflect.Struct, structcodec).
		RegisterDefaultDecoder(reflect.Map, bsoncodec.NewMapCodec(bsonoptions.MapCodec().SetDecodeZerosMap(true))).
		RegisterDefaultEncoder(reflect.Struct, structcodec).
		RegisterTypeMapEntry(bsontype.Int32, tInt).
		RegisterTypeMapEntry(bsontype.Type(0), tM).
		RegisterTypeMapEntry(bsontype.Binary, tByteSlice).
		RegisterTypeMapEntry(bsontype.DateTime, tTime).
		RegisterTypeMapEntry(bsontype.Array, tInterfaceSlice)

	return rb
}
