// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoimport

import (
	"encoding/base32"
	"encoding/base64"
	"encoding/hex"
	"fmt"
	"math"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/mongodb/mongo-tools/mongoimport/dateconv"
	"gopkg.in/mgo.v2/bson"
)

// columnType defines different types for columns that can be parsed distinctly
type columnType int

const (
	ctAuto columnType = iota
	ctBinary
	ctBoolean
	ctDate
	ctDateGo
	ctDateMS
	ctDateOracle
	ctDouble
	ctInt32
	ctInt64
	ctDecimal
	ctString
)

var (
	columnTypeRE      = regexp.MustCompile(`(?s)^(.*)\.(\w+)\((.*)\)$`)
	columnTypeNameMap = map[string]columnType{
		"auto":        ctAuto,
		"binary":      ctBinary,
		"boolean":     ctBoolean,
		"date":        ctDate,
		"decimal":     ctDecimal,
		"date_go":     ctDateGo,
		"date_ms":     ctDateMS,
		"date_oracle": ctDateOracle,
		"double":      ctDouble,
		"int32":       ctInt32,
		"int64":       ctInt64,
		"string":      ctString,
	}
)

type binaryEncoding int

const (
	beBase64 binaryEncoding = iota
	beBase32
	beHex
)

var binaryEncodingNameMap = map[string]binaryEncoding{
	"base64": beBase64,
	"base32": beBase32,
	"hex":    beHex,
}

// ColumnSpec keeps information for each 'column' of import.
type ColumnSpec struct {
	Name       string
	Parser     FieldParser
	ParseGrace ParseGrace
	TypeName   string
}

// ColumnNames maps a ColumnSpec slice to their associated names
func ColumnNames(fs []ColumnSpec) (s []string) {
	for _, f := range fs {
		s = append(s, f.Name)
	}
	return
}

// ParseTypedHeader produces a ColumnSpec from a header item, extracting type
// information from the it. The parseGrace is passed along to the new ColumnSpec.
func ParseTypedHeader(header string, parseGrace ParseGrace) (f ColumnSpec, err error) {
	match := columnTypeRE.FindStringSubmatch(header)
	if len(match) != 4 {
		err = fmt.Errorf("could not parse type from header %s", header)
		return
	}
	t, ok := columnTypeNameMap[match[2]]
	if !ok {
		err = fmt.Errorf("invalid type %s in header %s", match[2], header)
		return
	}
	p, err := NewFieldParser(t, match[3])
	if err != nil {
		return
	}
	return ColumnSpec{match[1], p, parseGrace, match[2]}, nil
}

// ParseTypedHeaders performs ParseTypedHeader on each item, returning an
// error if any single one fails.
func ParseTypedHeaders(headers []string, parseGrace ParseGrace) (fs []ColumnSpec, err error) {
	fs = make([]ColumnSpec, len(headers))
	for i, f := range headers {
		fs[i], err = ParseTypedHeader(f, parseGrace)
		if err != nil {
			return
		}
	}
	return
}

// ParseAutoHeaders converts a list of header items to ColumnSpec objects, with
// automatic parsers.
func ParseAutoHeaders(headers []string) (fs []ColumnSpec) {
	fs = make([]ColumnSpec, len(headers))
	for i, f := range headers {
		fs[i] = ColumnSpec{f, new(FieldAutoParser), pgAutoCast, "auto"}
	}
	return
}

// FieldParser is the interface for any parser of a field item.
type FieldParser interface {
	Parse(in string) (interface{}, error)
}

var (
	escapeReplacements = []string{
		`\\`, `\`,
		`\(`, "(",
		`\)`, ")",
		`\`, "",
	}
	escapeReplacer = strings.NewReplacer(escapeReplacements...)
)

// NewFieldParser yields a FieldParser corresponding to the given columnType.
// arg is passed along to the specific type's parser, if it permits an
// argument. An error will be raised if arg is not valid for the type's
// parser.
func NewFieldParser(t columnType, arg string) (parser FieldParser, err error) {
	arg = escapeReplacer.Replace(arg)

	switch t { // validate argument
	case ctBinary:
	case ctDate:
	case ctDateGo:
	case ctDateMS:
	case ctDateOracle:
	default:
		if arg != "" {
			err = fmt.Errorf("type %v does not support arguments", t)
			return
		}
	}

	switch t {
	case ctBinary:
		parser, err = NewFieldBinaryParser(arg)
	case ctBoolean:
		parser = new(FieldBooleanParser)
	case ctDate:
		fallthrough
	case ctDateGo:
		parser = &FieldDateParser{arg}
	case ctDateMS:
		parser = &FieldDateParser{dateconv.FromMS(arg)}
	case ctDateOracle:
		parser = &FieldDateParser{dateconv.FromOracle(arg)}
	case ctDouble:
		parser = new(FieldDoubleParser)
	case ctInt32:
		parser = new(FieldInt32Parser)
	case ctInt64:
		parser = new(FieldInt64Parser)
	case ctDecimal:
		parser = new(FieldDecimalParser)
	case ctString:
		parser = new(FieldStringParser)
	default: // ctAuto
		parser = new(FieldAutoParser)
	}
	return
}

func autoParse(in string) interface{} {
	parsedInt, err := strconv.ParseInt(in, 10, 64)
	if err == nil {
		if math.MinInt32 <= parsedInt && parsedInt <= math.MaxInt32 {
			return int32(parsedInt)
		}
		return parsedInt
	}
	parsedFloat, err := strconv.ParseFloat(in, 64)
	if err == nil {
		return parsedFloat
	}
	return in
}

type FieldAutoParser struct{}

func (ap *FieldAutoParser) Parse(in string) (interface{}, error) {
	return autoParse(in), nil
}

type FieldBinaryParser struct {
	enc binaryEncoding
}

func (bp *FieldBinaryParser) Parse(in string) (interface{}, error) {
	switch bp.enc {
	case beBase32:
		return base32.StdEncoding.DecodeString(in)
	case beBase64:
		return base64.StdEncoding.DecodeString(in)
	default: // beHex
		return hex.DecodeString(in)
	}
}

func NewFieldBinaryParser(arg string) (*FieldBinaryParser, error) {
	enc, ok := binaryEncodingNameMap[arg]
	if !ok {
		return nil, fmt.Errorf("invalid binary encoding: %s", arg)
	}
	return &FieldBinaryParser{enc}, nil
}

type FieldBooleanParser struct{}

func (bp *FieldBooleanParser) Parse(in string) (interface{}, error) {
	if strings.ToLower(in) == "true" || in == "1" {
		return true, nil
	}
	if strings.ToLower(in) == "false" || in == "0" {
		return false, nil
	}
	return nil, fmt.Errorf("failed to parse boolean: %s", in)
}

type FieldDateParser struct {
	layout string
}

func (dp *FieldDateParser) Parse(in string) (interface{}, error) {
	return time.Parse(dp.layout, in)
}

type FieldDoubleParser struct{}

func (dp *FieldDoubleParser) Parse(in string) (interface{}, error) {
	return strconv.ParseFloat(in, 64)
}

type FieldInt32Parser struct{}

func (ip *FieldInt32Parser) Parse(in string) (interface{}, error) {
	value, err := strconv.ParseInt(in, 10, 32)
	return int32(value), err
}

type FieldInt64Parser struct{}

func (ip *FieldInt64Parser) Parse(in string) (interface{}, error) {
	return strconv.ParseInt(in, 10, 64)
}

type FieldDecimalParser struct{}

func (ip *FieldDecimalParser) Parse(in string) (interface{}, error) {
	return bson.ParseDecimal128(in)
}

type FieldStringParser struct{}

func (sp *FieldStringParser) Parse(in string) (interface{}, error) {
	return in, nil
}
