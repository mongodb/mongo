package mongofiles

import (
	"bytes"
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/json"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"
	"github.com/mongodb/mongo-tools/common/util"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2"
	"io"
	"io/ioutil"
	"os"
	"strings"
	"testing"
)

var (
	testDB     = "mongofiles_test_db"
	testServer = "localhost"
	testPort   = db.DefaultTestPort

	ssl        = testutil.GetSSLOptions()
	auth       = testutil.GetAuthOptions()
	connection = &options.Connection{
		Host: testServer,
		Port: testPort,
	}
	toolOptions = &options.ToolOptions{
		SSL:        &ssl,
		Connection: connection,
		Auth:       &auth,
		Verbosity:  &options.Verbosity{},
		URI:        &options.URI{},
	}
)

// put in some test data into GridFS
func setUpGridFSTestData() ([]interface{}, error) {
	sessionProvider, err := db.NewSessionProvider(*toolOptions)
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
	sessionProvider, err := db.NewSessionProvider(*toolOptions)
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

func simpleMongoFilesInstance(args []string) (*MongoFiles, error) {
	sessionProvider, err := db.NewSessionProvider(*toolOptions)
	if err != nil {
		return nil, err
	}

	mongofiles := MongoFiles{
		ToolOptions:     toolOptions,
		InputOptions:    &InputOptions{},
		StorageOptions:  &StorageOptions{GridFSPrefix: "fs", DB: testDB},
		SessionProvider: sessionProvider,
		Command:         args[0],
		FileName:        args[1],
	}

	return &mongofiles, nil
}

// get an id of an existing file, for _id access
func idOfFile(mf *MongoFiles, filename string) string {
	session, _ := mf.SessionProvider.GetSession()
	defer session.Close()
	gfs := session.DB(mf.StorageOptions.DB).GridFS(mf.StorageOptions.GridFSPrefix)
	gFile, _ := gfs.Open(filename)
	bytes, _ := json.Marshal(gFile.Id())
	return fmt.Sprintf("ObjectId(%v)", string(bytes))
}

// test output needs some cleaning
func cleanAndTokenizeTestOutput(str string) []string {
	// remove last \r\n in str to avoid unnecessary line on split
	if str != "" {
		str = str[:len(str)-1]
	}

	return strings.Split(strings.Trim(str, "\r\n"), "\n")
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

// Test that it works whenever valid arguments are passed in and that
// it barfs whenever invalid ones are passed
func TestValidArguments(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("With a MongoFiles instance", t, func() {
		args := []string{"search", "file"}
		mf, err := simpleMongoFilesInstance(args)
		So(err, ShouldBeNil)
		Convey("It should error out when no arguments fed", func() {
			args := []string{}
			err := mf.ValidateCommand(args)
			So(err, ShouldNotBeNil)
			So(err.Error(), ShouldEqual, "no command specified")
		})

		Convey("It should error out when too many positional arguments provided", func() {
			args := []string{"list", "something", "another"}
			err := mf.ValidateCommand(args)
			So(err, ShouldNotBeNil)
			So(err.Error(), ShouldEqual, "too many positional arguments")
		})

		Convey("It should not error out when list command isn't given an argument", func() {
			args := []string{"list"}
			So(mf.ValidateCommand(args), ShouldBeNil)
			So(mf.StorageOptions.LocalFileName, ShouldEqual, "")
		})

		Convey("It should error out when any of (get|put|delete|search) not given supporting argument", func() {
			for _, command := range []string{"get", "put", "delete", "search"} {
				args := []string{command}
				err := mf.ValidateCommand(args)
				So(err, ShouldNotBeNil)
				So(err.Error(), ShouldEqual, fmt.Sprintf("'%v' argument missing", command))
			}
		})

		Convey("It should error out when a nonsensical command is given", func() {
			args := []string{"commandnonexistent"}

			err := mf.ValidateCommand(args)
			So(err, ShouldNotBeNil)
			So(err.Error(), ShouldEqual, fmt.Sprintf("'%v' is not a valid command", args[0]))
		})

	})
}

// Test that the output from mongofiles is actually correct
func TestMongoFilesCommands(t *testing.T) {
	testutil.VerifyTestType(t, testutil.IntegrationTestType)

	Convey("Testing the various commands (get|get_id|put|delete|delete_id|search|list) "+
		"with a MongoDump instance", t, func() {

		bytesExpected, err := setUpGridFSTestData()
		So(err, ShouldBeNil)

		// []interface{} here so we can use 'ensureSetEquality' method for both []string and []int
		filesExpected := []interface{}{"testfile1", "testfile2", "testfile3"}

		Convey("Testing the 'list' command with a file that isn't in GridFS should", func() {
			args := []string{"list", "gibberish"}

			mf, err := simpleMongoFilesInstance(args)
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

			mf, err := simpleMongoFilesInstance(args)
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

			mf, err := simpleMongoFilesInstance(args)
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

			mf, err := simpleMongoFilesInstance(args)
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

		Convey("Testing the 'get_id' command with a file that is in GridFS should", func() {
			// hack to grab an _id
			args := []string{"get", "testfile1"}
			mf, _ := simpleMongoFilesInstance(args)
			idString := idOfFile(mf, "testfile1")

			args = []string{"get_id", idString}
			mf, err = simpleMongoFilesInstance(args)
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
			args := []string{"put", "lorem_ipsum_287613_bytes.txt"}

			mf, err := simpleMongoFilesInstance(args)
			So(err, ShouldBeNil)
			So(mf, ShouldNotBeNil)
			mf.StorageOptions.LocalFileName = util.ToUniversalPath("testdata/lorem_ipsum_287613_bytes.txt")

			Convey("insert the file by creating two chunks (ceil(287,613 / 255 * 1024)) in GridFS", func() {
				str, err := mf.Run(false)
				So(err, ShouldBeNil)
				So(len(str), ShouldNotEqual, 0)

				Convey("and should have exactly 287613 bytes", func() {
					args = []string{"list", ""}

					mfAfter, err := simpleMongoFilesInstance(args)
					So(err, ShouldBeNil)
					So(mf, ShouldNotBeNil)

					str, err = mfAfter.Run(false)
					So(err, ShouldBeNil)

					lines := cleanAndTokenizeTestOutput(str)
					filesGotten, _ := getFilesAndBytesFromLines(lines)
					So(len(lines), ShouldEqual, len(filesExpected)+1)
					So(filesGotten, ShouldContain, "lorem_ipsum_287613_bytes.txt")
				})

				Convey("and should have exactly the same content as the original file", func() {
					args = []string{"get", "lorem_ipsum_287613_bytes.txt"}
					So(err, ShouldBeNil)
					mfAfter, err := simpleMongoFilesInstance(args)
					So(err, ShouldBeNil)
					So(mf, ShouldNotBeNil)

					mfAfter.StorageOptions.LocalFileName = "lorem_ipsum_copy.txt"
					str, err = mfAfter.Run(false)
					So(err, ShouldBeNil)
					So(len(str), ShouldNotEqual, 0)

					loremIpsumOrig, err := os.Open(util.ToUniversalPath("testdata/lorem_ipsum_287613_bytes.txt"))
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

			mf, err := simpleMongoFilesInstance(args)
			So(err, ShouldBeNil)
			So(mf, ShouldNotBeNil)

			Convey("delete the file from GridFS", func() {
				str, err := mf.Run(false)
				So(err, ShouldBeNil)
				So(len(str), ShouldNotEqual, 0)

				Convey("check that the file has been deleted from GridFS", func() {
					args = []string{"list", ""}
					mfAfter, err := simpleMongoFilesInstance(args)
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

		Convey("Testing the 'delete_id' command with a file that is in GridFS should", func() {
			// hack to grab an _id
			args := []string{"get", "testfile2"}
			mf, _ := simpleMongoFilesInstance(args)
			idString := idOfFile(mf, "testfile2")

			args = []string{"delete_id", idString}
			mf, err := simpleMongoFilesInstance(args)
			So(err, ShouldBeNil)
			So(mf, ShouldNotBeNil)

			Convey("delete the file from GridFS", func() {
				str, err := mf.Run(false)
				So(err, ShouldBeNil)
				So(len(str), ShouldNotEqual, 0)

				Convey("check that the file has been deleted from GridFS", func() {
					args = []string{"list", ""}
					mfAfter, err := simpleMongoFilesInstance(args)
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

}
