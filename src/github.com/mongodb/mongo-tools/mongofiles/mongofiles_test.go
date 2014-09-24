package mongofiles

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	commonOpts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/mongofiles/options"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2"
	"io/ioutil"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

var (
	testDB     = "mongofiles_test_db"
	testServer = "localhost"
	testPort   = "27017"

	ssl = &commonOpts.SSL{
		UseSSL: false,
	}
	namespace = &commonOpts.Namespace{
		DB: testDB,
	}
	connection = &commonOpts.Connection{
		Host: testServer,
		Port: testPort,
	}
	toolOptions = &commonOpts.ToolOptions{
		SSL:        ssl,
		Namespace:  namespace,
		Connection: connection,
		Auth:       &commonOpts.Auth{},
	}
)

// put in some test data into GridFS
func setUpGridFSTestData() ([]int, error) {
	sessionProvider, err := db.InitSessionProvider(*toolOptions)
	if err != nil {
		return nil, err
	}
	session, err := sessionProvider.GetSession()
	if err != nil {
		return nil, err
	}
	defer session.Close()

	bytesWritten := []int{}
	gfs := session.DB(testDB).GridFS("fs")
	var testfile *mgo.GridFile
	for i, item := range []string{"testfile1", "testfile2", "testfile3"} {
		testfile, err = gfs.Create(item)
		if err != nil {
			return nil, err
		}
		defer testfile.Close()
		n, err := testfile.Write([]byte(strings.Repeat("a", (i+1)*5)))
		if err != nil {
			return nil, err
		}

		bytesWritten = append(bytesWritten, n)
	}

	return bytesWritten, nil
}

// remove test data from GridFS
func tearDownGridFSTestData() error {
	sessionProvider, err := db.InitSessionProvider(*toolOptions)
	if err != nil {
		return err
	}
	session, err := sessionProvider.GetSession()
	if err != nil {
		return err
	}
	defer session.Close()
	if err = session.DB(testDB).DropDatabase(); err != nil {
		return err
	}

	return nil
}

// mongofiles instance that doesn't use the shim
func driverMongoFilesInstance(args []string) (*MongoFiles, error) {
	mongofiles := MongoFiles{
		ToolOptions:    toolOptions,
		StorageOptions: &options.StorageOptions{},
		Command:        args[0],
		FileName:       args[1],
	}
	if err := mongofiles.Init(); err != nil {
		return nil, err
	}

	return &mongofiles, nil
}

func cleanAndTokenizeTestOutput(str string) []string {
	// remove last \n in str to avoid unnecessary line on split
	if str != "" {
		str = str[:len(str)-1]
	}
	return strings.Split(strings.Trim(str, "\n"), "\n")
}

// mongofiles instance that uses the shim
func shimMongoFilesInstance(args []string) (*MongoFiles, error) {
	mongofiles := MongoFiles{
		ToolOptions:    toolOptions,
		StorageOptions: &options.StorageOptions{},
		Command:        args[0],
		FileName:       args[1],
	}
	path, err := os.Getwd()
	if err != nil {
		return nil, err
	}

	mongofiles.ToolOptions.Namespace.DB = "shimtest"
	mongofiles.ToolOptions.Namespace.DBPath = filepath.Join(path, "testdata")

	if err := mongofiles.Init(); err != nil {
		return nil, err
	}

	return &mongofiles, nil
}

// Test that it works whenever valid arguments are passed in and that
// it barfs whenever invalid ones are passed
func TestValidArguments(t *testing.T) {
	Convey("With a MongoFiles instance", t, func() {

		Convey("It should error out when no arguments fed", func() {
			args := []string{}
			_, err := ValidateCommand(args)
			So(err, ShouldNotBeNil)
			So(err.Error(), ShouldEqual, "you must specify a command")
		})

		Convey("It should error out when too many positional arguments provided", func() {
			args := []string{"list", "something", "another"}
			_, err := ValidateCommand(args)
			So(err, ShouldNotBeNil)
			So(err.Error(), ShouldEqual, "too many positional arguments")
		})

		Convey("It should not error out when list command isn't given an argument", func() {
			args := []string{"list"}
			fileName, err := ValidateCommand(args)
			So(err, ShouldBeNil)
			So(fileName, ShouldEqual, "")
		})

		Convey("It should error out when any of (get|put|delete|search) not given supporting argument",
			func() {

				var args []string
				for _, command := range []string{"get", "put", "delete", "search"} {
					args = []string{command}
					_, err := ValidateCommand(args)
					So(err, ShouldNotBeNil)
					So(err.Error(),
						ShouldEqual,
						fmt.Sprintf("'%v' requires a non-empty supporting argument",
							command),
					)
				}

			})

		Convey("It should error out when a nonsensical command is given", func() {
			args := []string{"commandnonexistent"}
			_, err := ValidateCommand(args)
			So(err, ShouldNotBeNil)
			So(err.Error(), ShouldEqual, fmt.Sprintf("'%v' is not a valid command", args[0]))
		})
	})
}

// Test that the output from mongofiles is actually correct
func TestMongoFilesCommands(t *testing.T) {
	Convey("With a MongoFiles instance (THAT DOESN'T USE THE SHIM) testing the various commands:(get|put|delete|search|list)", t, func() {
		bytesWritten, err := setUpGridFSTestData()
		So(err, ShouldBeNil)

		Convey("Testing the 'list' command with no output should", func() {
			args := []string{"list", "gibberish"}

			mf, err := driverMongoFilesInstance(args)
			So(mf, ShouldNotBeNil)
			So(err, ShouldBeNil)

			output, err := mf.Run()
			So(err, ShouldBeNil)
			So(len(output), ShouldEqual, 0)
		})

		Convey("Testing the 'list' command with some output should", func() {
			args := []string{"list", "testf"}

			mf, err := driverMongoFilesInstance(args)
			So(mf, ShouldNotBeNil)
			So(err, ShouldBeNil)

			str, err := mf.Run()
			So(err, ShouldBeNil)
			So(len(str), ShouldNotEqual, 0)

			filesExpected := []string{"testfile1", "testfile2", "testfile3"}
			lines := cleanAndTokenizeTestOutput(str)
			So(len(lines), ShouldEqual, len(filesExpected))

			var fileName string
			var byteCount int
			for _, line := range lines {
				fmt.Sscanf(line, "%s\t%d", &fileName, &byteCount)
				So(filesExpected, ShouldContain, fileName)
				So(bytesWritten, ShouldContain, byteCount)
			}
		})

		Convey("Testing the 'get' command with some output should", func() {
			args := []string{"get", "testfile1"}

			mf, err := driverMongoFilesInstance(args)
			So(mf, ShouldNotBeNil)
			So(err, ShouldBeNil)

			So(err, ShouldBeNil)
			str, err := mf.Run()
			So(err, ShouldBeNil)
			So(len(str), ShouldNotEqual, 0)

			testFile, err := os.Open("testfile1")
			So(err, ShouldBeNil)
			testFile1Bytes, err := ioutil.ReadAll(testFile)
			So(err, ShouldBeNil)
			So(len(testFile1Bytes), ShouldEqual, bytesWritten[0])

			Reset(func() {
				err = os.Remove("testfile1")
				So(err, ShouldBeNil)
			})
		})

		Convey("Testing the 'search' command with some output should", func() {
			args := []string{"search", "file"}

			mf, err := driverMongoFilesInstance(args)
			So(mf, ShouldNotBeNil)
			So(err, ShouldBeNil)

			str, err := mf.Run()
			So(err, ShouldBeNil)
			So(len(str), ShouldNotEqual, 0)

			filesExpected := []string{"testfile1", "testfile2", "testfile3"}
			lines := cleanAndTokenizeTestOutput(str)
			So(len(lines), ShouldEqual, len(filesExpected))

			var fileName string
			var byteCount int
			for _, line := range lines {
				fmt.Sscanf(line, "%s\t%d", &fileName, &byteCount)
				So(filesExpected, ShouldContain, fileName)
				So(bytesWritten, ShouldContain, byteCount)
			}
		})

		Reset(func() {
			So(tearDownGridFSTestData(), ShouldBeNil)
		})
	})

	Convey("With a MongoFiles instance (THAT USES THE SHIM) testing the various commands:(get|put|delete|search|list)", t, func() {

		filesExpected := []string{"samplefile1.txt", "samplefile2.txt", "samplefile3.txt"}
		bytesExpected := []int{71, 41, 51}

		Convey("Testing the 'list' command with no output should", func() {
			args := []string{"list", "gibberish"}

			mf, err := shimMongoFilesInstance(args)

			So(mf, ShouldNotBeNil)
			So(err, ShouldBeNil)

			output, err := mf.Run()
			So(err, ShouldBeNil)
			So(len(output), ShouldEqual, 0)
		})

		Convey("Testing the 'list' command with some output should", func() {
			args := []string{"list", "sample"}

			mf, err := shimMongoFilesInstance(args)

			So(mf, ShouldNotBeNil)
			So(err, ShouldBeNil)

			str, err := mf.Run()
			So(err, ShouldBeNil)

			lines := cleanAndTokenizeTestOutput(str)
			So(len(lines), ShouldEqual, len(filesExpected))

			var fileName string
			var byteCount int
			for _, line := range lines {
				fmt.Sscanf(line, "%s\t%d", &fileName, &byteCount)
				So(filesExpected, ShouldContain, fileName)
				So(bytesExpected, ShouldContain, byteCount)
			}
		})

		Convey("Testing the 'get' command with some output should", func() {
			args := []string{"get", "samplefile1.txt"}

			mf, err := shimMongoFilesInstance(args)

			So(mf, ShouldNotBeNil)
			So(err, ShouldBeNil)

			So(err, ShouldBeNil)
			str, err := mf.Run()
			So(err, ShouldBeNil)
			So(len(str), ShouldNotEqual, 0)

			testFile, err := os.Open("samplefile1.txt")
			So(err, ShouldBeNil)
			sampleFile1Bytes, err := ioutil.ReadAll(testFile)
			So(err, ShouldBeNil)
			So(len(sampleFile1Bytes), ShouldEqual, bytesExpected[0])

			Reset(func() {
				err = os.Remove("samplefile1.txt")
				So(err, ShouldBeNil)
			})
		})

	})
}
