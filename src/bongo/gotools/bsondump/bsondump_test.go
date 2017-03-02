package bsondump

import (
	"bytes"
	"os"
	"os/exec"
	"testing"

	. "github.com/smartystreets/goconvey/convey"
)

func TestBsondump(t *testing.T) {

	Convey("Test bsondump reading from stdin and writing to stdout", t, func() {
		cmd := exec.Command("../bin/bsondump")

		// Attach a file to stdin of the command.
		inFile, err := os.Open("testdata/sample.bson")
		So(err, ShouldBeNil)
		cmd.Stdin = inFile

		// Attach a buffer to stdout of the command.
		cmdOutput := &bytes.Buffer{}
		cmd.Stdout = cmdOutput

		err = cmd.Run()
		So(err, ShouldBeNil)

		// Get the correct bsondump result from a file to use as a reference.
		outReference, err := os.Open("testdata/sample.json")
		So(err, ShouldBeNil)
		bufRef := new(bytes.Buffer)
		bufRef.ReadFrom(outReference)
		bufRefStr := bufRef.String()

		bufDumpStr := cmdOutput.String()
		So(bufDumpStr, ShouldEqual, bufRefStr)
	})

	Convey("Test bsondump reading from stdin and writing to a file", t, func() {
		cmd := exec.Command("../bin/bsondump", "--outFile", "out.json")

		// Attach a file to stdin of the command.
		inFile, err := os.Open("testdata/sample.bson")
		So(err, ShouldBeNil)
		cmd.Stdin = inFile

		err = cmd.Run()
		So(err, ShouldBeNil)

		// Get the correct bsondump result from a file to use as a reference.
		outReference, err := os.Open("testdata/sample.json")
		So(err, ShouldBeNil)
		bufRef := new(bytes.Buffer)
		bufRef.ReadFrom(outReference)
		bufRefStr := bufRef.String()

		// Get the output from a file.
		outDump, err := os.Open("out.json")
		So(err, ShouldBeNil)
		bufDump := new(bytes.Buffer)
		bufDump.ReadFrom(outDump)
		bufDumpStr := bufDump.String()

		So(bufDumpStr, ShouldEqual, bufRefStr)
	})

	Convey("Test bsondump reading from a file with --bsonFile and writing to stdout", t, func() {
		cmd := exec.Command("../bin/bsondump", "--bsonFile", "testdata/sample.bson")

		// Attach a buffer to stdout of the command.
		cmdOutput := &bytes.Buffer{}
		cmd.Stdout = cmdOutput

		err := cmd.Run()
		So(err, ShouldBeNil)

		// Get the correct bsondump result from a file to use as a reference.
		outReference, err := os.Open("testdata/sample.json")
		So(err, ShouldBeNil)
		bufRef := new(bytes.Buffer)
		bufRef.ReadFrom(outReference)
		bufRefStr := bufRef.String()

		bufDumpStr := cmdOutput.String()
		So(bufDumpStr, ShouldEqual, bufRefStr)
	})

	Convey("Test bsondump reading from a file with a positional arg and writing to stdout", t, func() {
		cmd := exec.Command("../bin/bsondump", "testdata/sample.bson")

		// Attach a buffer to stdout of command.
		cmdOutput := &bytes.Buffer{}
		cmd.Stdout = cmdOutput

		err := cmd.Run()
		So(err, ShouldBeNil)

		// Get the correct bsondump result from a file to use as a reference.
		outReference, err := os.Open("testdata/sample.json")
		So(err, ShouldBeNil)
		bufRef := new(bytes.Buffer)
		bufRef.ReadFrom(outReference)
		bufRefStr := bufRef.String()

		bufDumpStr := cmdOutput.String()
		So(bufDumpStr, ShouldEqual, bufRefStr)
	})

	Convey("Test bsondump reading from a file with --bsonFile and writing to a file", t, func() {
		cmd := exec.Command("../bin/bsondump", "--outFile", "out.json",
			"--bsonFile", "testdata/sample.bson")

		err := cmd.Run()
		So(err, ShouldBeNil)

		// Get the correct bsondump result from a file to use as a reference.
		outReference, err := os.Open("testdata/sample.json")
		So(err, ShouldBeNil)
		bufRef := new(bytes.Buffer)
		bufRef.ReadFrom(outReference)
		bufRefStr := bufRef.String()

		// Get the output from a file.
		outDump, err := os.Open("out.json")
		So(err, ShouldBeNil)
		bufDump := new(bytes.Buffer)
		bufDump.ReadFrom(outDump)
		bufDumpStr := bufDump.String()

		So(bufDumpStr, ShouldEqual, bufRefStr)
	})

	Convey("Test bsondump reading from a file with a positional arg and writing to a file", t, func() {
		cmd := exec.Command("../bin/bsondump", "--outFile", "out.json", "testdata/sample.bson")

		err := cmd.Run()
		So(err, ShouldBeNil)

		// Get the correct bsondump result from a file to use as a reference.
		outReference, err := os.Open("testdata/sample.json")
		So(err, ShouldBeNil)
		bufRef := new(bytes.Buffer)
		bufRef.ReadFrom(outReference)
		bufRefStr := bufRef.String()

		// Get the output from a file.
		outDump, err := os.Open("out.json")
		So(err, ShouldBeNil)
		bufDump := new(bytes.Buffer)
		bufDump.ReadFrom(outDump)
		bufDumpStr := bufDump.String()

		So(bufDumpStr, ShouldEqual, bufRefStr)
	})
}
