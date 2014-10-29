package mongodump

import (
	"github.com/mongodb/mongo-tools/common/testutil"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongodump/options"
	. "github.com/smartystreets/goconvey/convey"
	"os"
	"path/filepath"
	"testing"
)

const (
	KERBEROS_DUMP_DIRECTORY = "dump-kerberos"
)

func TestMongoDumpKerberos(t *testing.T) {
	testutil.VerifyTestType(t, testutil.KERBEROS_TEST_TYPE)

	Convey("Should be able to run mongodump with Kerberos auth", t, func() {
		opts, err := testutil.GetKerberosOptions()

		So(err, ShouldBeNil)

		mongoDump := MongoDump{
			ToolOptions:   opts,
			InputOptions:  &options.InputOptions{},
			OutputOptions: &options.OutputOptions{},
		}

		mongoDump.OutputOptions.Out = KERBEROS_DUMP_DIRECTORY

		err = mongoDump.Init()
		So(err, ShouldBeNil)
		err = mongoDump.Dump()
		So(err, ShouldBeNil)
		path, err := os.Getwd()
		So(err, ShouldBeNil)

		dumpDir := util.ToUniversalPath(filepath.Join(path, KERBEROS_DUMP_DIRECTORY))
		dumpDBDir := util.ToUniversalPath(filepath.Join(dumpDir, opts.Namespace.DB))
		So(fileDirExists(dumpDir), ShouldBeTrue)
		So(fileDirExists(dumpDBDir), ShouldBeTrue)

		dumpCollectionFile := util.ToUniversalPath(filepath.Join(dumpDBDir, opts.Namespace.Collection+".bson"))
		So(fileDirExists(dumpCollectionFile), ShouldBeTrue)

		countColls, err := countNonIndexBSONFiles(dumpDBDir)
		So(err, ShouldBeNil)
		So(countColls, ShouldEqual, 1)
	})
}
