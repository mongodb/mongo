package mongofiles

import (
	"bytes"
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	commonOpts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/mongofiles/options"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2"
	"io"
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
func setUpGridFSTestData() ([]interface{}, error) {
	sessionProvider, err := db.InitSessionProvider(*toolOptions)
	if err != nil {
		return nil, err
	}
	session, err := sessionProvider.GetSession()
	if err != nil {
		return nil, err
	}
	defer session.Close()

	bytesExpected := []interface{}{}
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

		bytesExpected = append(bytesExpected, n)
	}

	return bytesExpected, nil
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

// test output needs some cleaning
func cleanAndTokenizeTestOutput(str string) []string {
	// remove last \n in str to avoid unnecessary line on split
	if str != "" {
		str = str[:len(str)-1]
	}

	return strings.Split(strings.Trim(str, "\n"), "\n")
}

// return slices of files and bytes in each file represented by each line
func getFilesAndBytesFromLines(lines []string) ([]interface{}, []interface{}) {
	var fileName string
	var byteCount int

	filesGotten := []interface{}{}
	bytesGotten := []interface{}{}

	for _, line := range lines {
		fmt.Sscanf(line, "%s\t%d", &fileName, &byteCount)

		filesGotten = append(filesGotten, fileName)
		bytesGotten = append(bytesGotten, byteCount)
	}

	return filesGotten, bytesGotten
}

// inefficient but fast way to ensure set equality of
func ensureSetEquality(firstArray []interface{}, secondArray []interface{}) {
	for _, line := range firstArray {
		So(secondArray, ShouldContain, line)
	}
}

// check if file exists
func fileExists(name string) bool {
	if _, err := os.Stat(name); err != nil {
		if os.IsNotExist(err) {
			return false
		}
	}
	return true
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

	// for testing mongofiles through mongoshim,
	// we'll use a database called 'shimtest'
	mongofiles.ToolOptions.Namespace.DB = "shimtest"
	mongofiles.ToolOptions.Namespace.DBPath = filepath.Join(path, "testdata/datafiles")

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

		Convey("It should error out when any of (get|put|delete|search) not given supporting argument", func() {
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

		bytesExpected, err := setUpGridFSTestData()
		So(err, ShouldBeNil)

		// []interface{} here so we can use 'ensureSetEquality' method for both []string and []int
		filesExpected := []interface{}{"testfile1", "testfile2", "testfile3"}

		Convey("Testing the 'list' command with a file that isn't in GridFS should", func() {
			args := []string{"list", "gibberish"}

			mf, err := driverMongoFilesInstance(args)
			So(err, ShouldBeNil)
			So(mf, ShouldNotBeNil)

			Convey("produce no output", func() {
				output, err := mf.Run(false)
				So(err, ShouldBeNil)
				So(len(output), ShouldEqual, 0)
			})
		})

		Convey("Testing the 'list' command with files that are in GridFS should", func() {
			args := []string{"list", "testf"}

			mf, err := driverMongoFilesInstance(args)
			So(err, ShouldBeNil)
			So(mf, ShouldNotBeNil)

			Convey("produce some output", func() {
				str, err := mf.Run(false)
				So(err, ShouldBeNil)
				So(len(str), ShouldNotEqual, 0)

				lines := cleanAndTokenizeTestOutput(str)
				So(len(lines), ShouldEqual, len(filesExpected))

				filesGotten, bytesGotten := getFilesAndBytesFromLines(lines)
				ensureSetEquality(filesExpected, filesGotten)
				ensureSetEquality(bytesExpected, bytesGotten)
			})
		})

		Convey("Testing the 'search' command with files that are in GridFS should", func() {
			args := []string{"search", "file"}

			mf, err := driverMongoFilesInstance(args)
			So(err, ShouldBeNil)
			So(mf, ShouldNotBeNil)

			Convey("produce some output", func() {
				str, err := mf.Run(false)
				So(err, ShouldBeNil)
				So(len(str), ShouldNotEqual, 0)

				lines := cleanAndTokenizeTestOutput(str)
				So(len(lines), ShouldEqual, len(filesExpected))

				filesGotten, bytesGotten := getFilesAndBytesFromLines(lines)
				ensureSetEquality(filesExpected, filesGotten)
				ensureSetEquality(bytesExpected, bytesGotten)
			})
		})

		Convey("Testing the 'get' command with a file that is in GridFS should", func() {
			args := []string{"get", "testfile1"}

			mf, err := driverMongoFilesInstance(args)
			So(err, ShouldBeNil)
			So(mf, ShouldNotBeNil)

			Convey("copy the file to the local filesystem", func() {
				str, err := mf.Run(false)
				So(err, ShouldBeNil)
				So(len(str), ShouldNotEqual, 0)

				testFile, err := os.Open("testfile1")
				So(err, ShouldBeNil)
				defer testFile.Close()

				// pretty small file; so read all
				testFile1Bytes, err := ioutil.ReadAll(testFile)
				So(err, ShouldBeNil)
				So(len(testFile1Bytes), ShouldEqual, bytesExpected[0])
			})

			Convey("store the file contents in a file with different name if '--local' flag used", func() {
				mf.StorageOptions.LocalFileName = "testfile1copy"
				str, err := mf.Run(false)
				So(err, ShouldBeNil)
				So(len(str), ShouldNotEqual, 0)

				testFile, err := os.Open("testfile1copy")
				So(err, ShouldBeNil)
				defer testFile.Close()

				// pretty small file; so read all
				testFile1Bytes, err := ioutil.ReadAll(testFile)
				So(err, ShouldBeNil)
				So(len(testFile1Bytes), ShouldEqual, bytesExpected[0])
			})

			// cleanup file we just copied to the local FS
			Reset(func() {

				// remove 'testfile1' or 'testfile1copy'
				if fileExists("testfile1") {
					err = os.Remove("testfile1")
				}
				So(err, ShouldBeNil)

				if fileExists("testfile1copy") {
					err = os.Remove("testfile1copy")
				}
				So(err, ShouldBeNil)

			})
		})

		Convey("Testing the 'put' command by putting some lorem ipsum file with 287613 bytes should", func() {
			args := []string{"put", "testdata/lorem_ipsum_287613_bytes.txt"}

			mf, err := driverMongoFilesInstance(args)
			So(err, ShouldBeNil)
			So(mf, ShouldNotBeNil)

			Convey("insert the file by creating two chunks (ceil(287,613 / 255 * 1024)) in GridFS", func() {
				str, err := mf.Run(false)
				So(err, ShouldBeNil)
				So(len(str), ShouldNotEqual, 0)

				Convey("and should have exactly 287613 bytes", func() {
					args = []string{"list", ""}

					mfAfter, err := driverMongoFilesInstance(args)
					So(err, ShouldBeNil)
					So(mf, ShouldNotBeNil)

					str, err = mfAfter.Run(false)
					So(err, ShouldBeNil)

					lines := cleanAndTokenizeTestOutput(str)
					filesGotten, bytesGotten := getFilesAndBytesFromLines(lines)
					So(len(lines), ShouldEqual, len(filesExpected)+1)

					So(filesGotten, ShouldContain, "testdata/lorem_ipsum_287613_bytes.txt")
					So(bytesGotten, ShouldContain, 287613)
				})

				Convey("and should have exactly the same content as the original file", func() {
					args = []string{"get", "testdata/lorem_ipsum_287613_bytes.txt"}
					So(err, ShouldBeNil)
					mfAfter, err := driverMongoFilesInstance(args)
					So(err, ShouldBeNil)
					So(mf, ShouldNotBeNil)

					mfAfter.StorageOptions.LocalFileName = "lorem_ipsum_copy.txt"
					str, err = mfAfter.Run(false)
					So(err, ShouldBeNil)
					So(len(str), ShouldNotEqual, 0)

					loremIpsumOrig, err := os.Open("testdata/lorem_ipsum_287613_bytes.txt")
					So(err, ShouldBeNil)

					loremIpsumCopy, err := os.Open("lorem_ipsum_copy.txt")
					So(err, ShouldBeNil)

					Convey("compare the copy of the lorem ipsum file with the original 1KB at a time", func() {
						dataBytesOrig := make([]byte, 1024)
						dataBytesCopy := make([]byte, 1024)

						defer loremIpsumOrig.Close()
						defer loremIpsumCopy.Close()

						var nReadOrig, nReadCopy int

						for {
							nReadOrig, err = loremIpsumOrig.Read(dataBytesOrig)

							// err should either be nil
							// or io.EOF --> indicating end of file
							So(err, ShouldBeIn, []error{nil, io.EOF})

							if nReadOrig == 0 {
								break
							}

							nReadCopy, err = loremIpsumCopy.Read(dataBytesCopy)
							So(err, ShouldBeNil)

							So(nReadOrig, ShouldEqual, nReadCopy)
							So(bytes.Compare(dataBytesOrig, dataBytesCopy), ShouldEqual, 0)
						}
					})

					Reset(func() {
						err = os.Remove("lorem_ipsum_copy.txt")
						So(err, ShouldBeNil)
					})

				})

			})

		})

		Convey("Testing the 'delete' command with a file that is in GridFS should", func() {
			args := []string{"delete", "testfile2"}

			mf, err := driverMongoFilesInstance(args)
			So(err, ShouldBeNil)
			So(mf, ShouldNotBeNil)

			Convey("delete the file from GridFS", func() {
				str, err := mf.Run(false)
				So(err, ShouldBeNil)
				So(len(str), ShouldNotEqual, 0)

				Convey("check that the file has been deleted from GridFS", func() {
					args = []string{"list", ""}
					mfAfter, err := driverMongoFilesInstance(args)
					So(err, ShouldBeNil)
					So(mf, ShouldNotBeNil)

					str, err = mfAfter.Run(false)
					So(err, ShouldBeNil)

					lines := cleanAndTokenizeTestOutput(str)
					So(len(lines), ShouldEqual, len(filesExpected)-1)

					filesGotten, bytesGotten := getFilesAndBytesFromLines(lines)

					So(filesGotten, ShouldNotContain, "testfile2")
					So(bytesGotten, ShouldNotContain, bytesExpected[1])
				})
			})
		})

		Reset(func() {
			So(tearDownGridFSTestData(), ShouldBeNil)
		})
	})

	Convey("With a MongoFiles instance (THAT USES THE SHIM) testing the various commands:(get|put|delete|search|list)", t, func() {

		// both []interface{} so we can use 'ensureSetEquality' on both []string and []int
		filesExpected := []interface{}{"samplefile1.txt", "samplefile2.txt", "samplefile3.txt"}
		bytesExpected := []interface{}{45, 41, 80}

		Convey("Testing the 'list' command with a file that isn't in GridFS should", func() {
			args := []string{"list", "gibberish"}

			mf, err := shimMongoFilesInstance(args)

			So(err, ShouldBeNil)
			So(mf, ShouldNotBeNil)

			Convey("produce no output", func() {
				output, err := mf.Run(false)
				So(err, ShouldBeNil)
				So(len(output), ShouldEqual, 0)
			})
		})

		Convey("Testing the 'list' command with files that are in GridFS should", func() {
			args := []string{"list", "sample"}

			mf, err := shimMongoFilesInstance(args)

			So(err, ShouldBeNil)
			So(mf, ShouldNotBeNil)

			Convey("produce some output", func() {
				str, err := mf.Run(false)
				So(err, ShouldBeNil)

				lines := cleanAndTokenizeTestOutput(str)
				So(len(lines), ShouldEqual, len(filesExpected))

				filesGotten, bytesGotten := getFilesAndBytesFromLines(lines)
				ensureSetEquality(filesExpected, filesGotten)
				ensureSetEquality(bytesExpected, bytesGotten)
			})
		})

		Convey("Testing the 'search' command with files that are in GridFS should", func() {
			args := []string{"search", "amplefile1"}

			mf, err := shimMongoFilesInstance(args)

			So(err, ShouldBeNil)
			So(mf, ShouldNotBeNil)

			Convey("produce some output", func() {
				str, err := mf.Run(false)
				So(err, ShouldBeNil)

				lines := cleanAndTokenizeTestOutput(str)
				So(len(lines), ShouldEqual, 1)

				filesGotten, bytesGotten := getFilesAndBytesFromLines(lines)

				So(filesGotten, ShouldContain, "samplefile1.txt")
				So(bytesGotten, ShouldContain, bytesExpected[0])
			})
		})

		Convey("Testing the 'get' command with a file that is in GridFS should", func() {
			args := []string{"get", "samplefile1.txt"}

			mf, err := shimMongoFilesInstance(args)

			So(err, ShouldBeNil)
			So(mf, ShouldNotBeNil)

			Convey("copy the file to the local filesystem", func() {
				str, err := mf.Run(false)
				So(err, ShouldBeNil)
				So(len(str), ShouldNotEqual, 0)

				testFile, err := os.Open("samplefile1.txt")
				So(err, ShouldBeNil)
				defer testFile.Close()

				// pretty small file, so let's read all
				sampleFile1Bytes, err := ioutil.ReadAll(testFile)
				So(err, ShouldBeNil)
				So(len(sampleFile1Bytes), ShouldEqual, bytesExpected[0])

				Reset(func() {
					err = os.Remove("samplefile1.txt")
					So(err, ShouldBeNil)
				})
			})
		})

	})
}
