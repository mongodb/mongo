// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package util

import (
	"bufio"
	"io"
	"net/url"
	"os"
	"path/filepath"
	"context"

	"go.mongodb.org/mongo-driver/mongo"
)

// GetFieldsFromFile fetches the first line from the contents of the file
// at "path"
func GetFieldsFromFile(path string) ([]string, error) {
	fieldFileReader, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer fieldFileReader.Close()

	var fields []string
	fieldScanner := bufio.NewScanner(fieldFileReader)
	for fieldScanner.Scan() {
		fields = append(fields, fieldScanner.Text())
	}
	if err := fieldScanner.Err(); err != nil {
		return nil, err
	}
	return fields, nil
}

// ToUniversalPath returns the result of replacing each slash ('/') character
// in "path" with an OS-sepcific separator character. Multiple slashes are
// replaced by multiple separators
func ToUniversalPath(path string) string {
	return filepath.FromSlash(path)
}

func EscapeCollectionName(collName string) string {
	return url.PathEscape(collName)
}

func UnescapeCollectionName(escapedCollName string) (string, error) {
	return url.PathUnescape(escapedCollName)
}

type WrappedReadCloser struct {
	io.ReadCloser
	Inner io.ReadCloser
}

func (wrc *WrappedReadCloser) Close() error {
	outerErr := wrc.ReadCloser.Close()
	innerErr := wrc.Inner.Close()
	if outerErr != nil {
		return outerErr
	}
	return innerErr
}

type WrappedWriteCloser struct {
	io.WriteCloser
	Inner io.WriteCloser
}

func (wwc *WrappedWriteCloser) Close() error {
	outerErr := wwc.WriteCloser.Close()
	innerErr := wwc.Inner.Close()
	if outerErr != nil {
		return outerErr
	}
	return innerErr
}


// Wrapper that can capture errors that occur when closing the underlying closer.
type DeferredCloser struct {
	io.Closer
	closed bool
}

// CloseWithErrorCapture closes the wrapped Closer and sets deferredErr to the error if one occurs.
// It will only assign an error deferredErr's pointee is nil and if the underlying Closer has not been closed yet.
func (dc *DeferredCloser) CloseWithErrorCapture(deferredErr *error) {
	if dc.closed {
		return
	}

	err := dc.Closer.Close()
	dc.closed = true
	if err != nil && *deferredErr == nil {
		*deferredErr = err
	}
}

// Wrapper around Cursor to implement Closer
type CloserCursor struct {
	*mongo.Cursor
}

func (cursor *CloserCursor) Close() error {
	return cursor.Cursor.Close(context.Background())
}
