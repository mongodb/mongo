// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package log

import (
	"bytes"
	"github.com/mongodb/mongo-tools/common/options"
	. "github.com/smartystreets/goconvey/convey"
	"os"
	"strings"
	"testing"
	"time"
)

func TestBasicToolLoggerFunctionality(t *testing.T) {
	var tl *ToolLogger

	oldTime := time.Now()
	// sleep to avoid failures due to low timestamp resolution
	time.Sleep(time.Millisecond)

	Convey("With a new ToolLogger", t, func() {
		v1 := &options.Verbosity{
			Quiet:        false,
			SetVerbosity: nil,
			VLevel:       3,
		}
		tl = NewToolLogger(v1)
		So(tl, ShouldNotBeNil)
		So(tl.writer, ShouldNotBeNil)
		So(tl.verbosity, ShouldEqual, 3)

		Convey("writing a negative verbosity should panic", func() {
			So(func() { tl.Logvf(-1, "nope") }, ShouldPanic)
		})

		Convey("writing the output to a buffer", func() {
			buf := bytes.NewBuffer(make([]byte, 1024))
			tl.SetWriter(buf)

			Convey("with Logfs of various verbosity levels", func() {
				tl.Logvf(0, "test this string")
				tl.Logvf(5, "this log level is too high and will not log")
				tl.Logvf(1, "====!%v!====", 12.5)

				Convey("only messages of low enough verbosity should be written", func() {
					l1, _ := buf.ReadString('\n')
					So(l1, ShouldContainSubstring, ":")
					So(l1, ShouldContainSubstring, ".")
					So(l1, ShouldContainSubstring, "test this string")
					l2, _ := buf.ReadString('\n')
					So(l2, ShouldContainSubstring, "====!12.5!====")

					Convey("and contain a proper timestamp", func() {
						So(l2, ShouldContainSubstring, "\t")
						timestamp := l2[:strings.Index(l2, "\t")]
						So(len(timestamp), ShouldBeGreaterThan, 1)
						parsedTime, err := time.Parse(ToolTimeFormat, timestamp)
						So(err, ShouldBeNil)
						So(parsedTime, ShouldHappenOnOrAfter, oldTime)
					})
				})
			})
		})
	})
}

func TestGlobalToolLoggerFunctionality(t *testing.T) {
	globalToolLogger = nil // just to be sure

	Convey("With an initialized global ToolLogger", t, func() {
		globalToolLogger = NewToolLogger(&options.Verbosity{
			Quiet:        false,
			SetVerbosity: nil,
			VLevel:       3,
		})
		So(globalToolLogger, ShouldNotBeNil)

		Convey("actions shouldn't panic", func() {
			So(func() { SetVerbosity(&options.Verbosity{Quiet: true}) }, ShouldNotPanic)
			So(func() { Logvf(0, "woooo") }, ShouldNotPanic)
			So(func() { SetDateFormat("ahaha") }, ShouldNotPanic)
			So(func() { SetWriter(os.Stdout) }, ShouldNotPanic)
		})
	})
}

func TestToolLoggerWriter(t *testing.T) {
	Convey("With a tool logger that writes to a buffer", t, func() {
		buff := bytes.NewBuffer(make([]byte, 1024))
		v1 := &options.Verbosity{
			Quiet:        false,
			SetVerbosity: nil,
			VLevel:       3,
		}
		tl := NewToolLogger(v1)
		tl.SetWriter(buff)

		Convey("writing using a ToolLogWriter", func() {
			tlw := tl.Writer(0)
			_, err := tlw.Write([]byte("One"))
			So(err, ShouldBeNil)
			_, err = tlw.Write([]byte("Two"))
			So(err, ShouldBeNil)
			_, err = tlw.Write([]byte("Three"))
			So(err, ShouldBeNil)

			Convey("the messages should appear in the buffer", func() {
				results := buff.String()
				So(results, ShouldContainSubstring, "One")
				So(results, ShouldContainSubstring, "Two")
				So(results, ShouldContainSubstring, "Three")
			})
		})

		Convey("but with a log writer of too high verbosity", func() {
			tlw2 := tl.Writer(1776)
			_, err := tlw2.Write([]byte("nothing to see here"))
			So(err, ShouldBeNil)

			Convey("nothing should be written", func() {
				results := buff.String()
				So(results, ShouldNotContainSubstring, "nothing")
			})
		})
	})
}
