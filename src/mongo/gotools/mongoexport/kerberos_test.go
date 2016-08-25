package mongoexport

import (
	"bytes"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/json"
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"strings"
	"testing"
)

func TestKerberos(t *testing.T) {
	testutil.VerifyTestType(t, testutil.KerberosTestType)

	Convey("Should be able to run mongoexport with Kerberos auth", t, func() {
		opts, err := testutil.GetKerberosOptions()

		So(err, ShouldBeNil)

		sessionProvider, err := db.NewSessionProvider(*opts)
		So(err, ShouldBeNil)

		export := MongoExport{
			ToolOptions:     *opts,
			OutputOpts:      &OutputFormatOptions{},
			InputOpts:       &InputOptions{},
			SessionProvider: sessionProvider,
		}

		var out bytes.Buffer
		num, err := export.exportInternal(&out)

		So(err, ShouldBeNil)
		So(num, ShouldEqual, 1)
		outputLines := strings.Split(strings.TrimSpace(out.String()), "\n")
		So(len(outputLines), ShouldEqual, 1)
		outMap := map[string]interface{}{}
		So(json.Unmarshal([]byte(outputLines[0]), &outMap), ShouldBeNil)
		So(outMap["kerberos"], ShouldEqual, true)
		So(outMap["authenticated"], ShouldEqual, "yeah")
		So(outMap["_id"].(map[string]interface{})["$oid"], ShouldEqual, "528fb35afb3a8030e2f643c3")
	})
}
