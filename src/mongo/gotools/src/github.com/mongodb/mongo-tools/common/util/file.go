// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package util

import (
	"bufio"
	"io"
	"os"
	"path/filepath"
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
