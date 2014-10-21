package mongoexport

import (
	"bytes"
	"github.com/mongodb/mongo-tools/common/db"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"
	"github.com/mongodb/mongo-tools/mongoexport/options"
	. "github.com/smartystreets/goconvey/convey"
	"strings"
	"testing"
)

func TestKerberos(t *testing.T) {
	testutil.VerifyTestType(t, testutil.KERBEROS_TEST_TYPE)

	var opts *commonopts.ToolOptions

	Convey("Should be able to run mongoexport with Kerberos auth", t, func() {
		opts = testutil.GetKerberosOptions()

		sessionProvider, err := db.InitSessionProvider(*opts)
		So(err, ShouldBeNil)

		export := MongoExport{
			ToolOptions:     *opts,
			OutputOpts:      &options.OutputFormatOptions{},
			InputOpts:       nil,
			SessionProvider: sessionProvider,
		}

		var out bytes.Buffer
		num, err := export.exportInternal(&out)

		So(err, ShouldBeNil)
		So(num, ShouldEqual, 1)
		outputLines := strings.Split(strings.TrimSpace(out.String()), "\n")
		So(len(outputLines), ShouldEqual, 1)
		So(outputLines[0], ShouldEqual,
			"{\"_id\":{\"$oid\":\"528fb35afb3a8030e2f643c3\"},"+
				"\"authenticated\":\"yeah\",\"kerberos\":true}")
	})
}
