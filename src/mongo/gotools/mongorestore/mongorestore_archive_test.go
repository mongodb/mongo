package mongorestore

import (
	"github.com/mongodb/mongo-tools/common/archive"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"
	"github.com/mongodb/mongo-tools/common/util"

	. "github.com/smartystreets/goconvey/convey"

	"io"
	"io/ioutil"
	"os"
	"testing"
)

func init() {
	// bump up the verbosity to make checking debug log output possible
	log.SetVerbosity(&options.Verbosity{
		VLevel: 4,
	})
}

var (
	testArchive = "testdata/test.bar.archive"
)

func TestMongorestoreShortArchive(t *testing.T) {
	Convey("With a test MongoRestore", t, func() {
		ssl := testutil.GetSSLOptions()
		auth := testutil.GetAuthOptions()

		testutil.VerifyTestType(t, testutil.IntegrationTestType)
		toolOptions := &options.ToolOptions{
			Connection: &options.Connection{
				Host: testServer,
				Port: testPort,
			},
			URI:  &options.URI{},
			Auth: &auth,
			SSL:  &ssl,
		}
		inputOptions := &InputOptions{
			Archive: testArchive,
		}
		outputOptions := &OutputOptions{
			NumParallelCollections: 1,
			NumInsertionWorkers:    1,
			WriteConcern:           "majority",
			Drop:                   true,
		}
		nsOptions := &NSOptions{}
		provider, err := db.NewSessionProvider(*toolOptions)
		if err != nil {
			log.Logvf(log.Always, "error connecting to host: %v", err)
			os.Exit(util.ExitError)
		}
		file, err := os.Open(testArchive)
		So(file, ShouldNotBeNil)
		So(err, ShouldBeNil)

		fi, err := file.Stat()
		So(fi, ShouldNotBeNil)
		So(err, ShouldBeNil)

		fileSize := fi.Size()

		for i := fileSize; i >= 0; i-- {

			log.Logvf(log.Always, "Restoring from the first %v bytes of a archive of size %v", i, fileSize)

			_, err = file.Seek(0, 0)
			So(err, ShouldBeNil)

			restore := MongoRestore{
				ToolOptions:     toolOptions,
				OutputOptions:   outputOptions,
				InputOptions:    inputOptions,
				NSOptions:       nsOptions,
				SessionProvider: provider,
				archive: &archive.Reader{
					Prelude: &archive.Prelude{},
					In:      ioutil.NopCloser(io.LimitReader(file, i)),
				},
			}
			err = restore.Restore()
			if i == fileSize {
				So(err, ShouldBeNil)
			} else {
				So(err, ShouldNotBeNil)
			}
		}
	})
}
