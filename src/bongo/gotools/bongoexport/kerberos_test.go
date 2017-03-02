package bongoexport

import (
	"bytes"
	"github.com/bongodb/bongo-tools/common/db"
	"github.com/bongodb/bongo-tools/common/json"
	"github.com/bongodb/bongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"strings"
	"testing"
)

func TestKerberos(t *testing.T) {
	testutil.VerifyTestType(t, testutil.KerberosTestType)

	Convey("Should be able to run bongoexport with Kerberos auth", t, func() {
		opts, err := testutil.GetKerberosOptions()

		So(err, ShouldBeNil)

		sessionProvider, err := db.NewSessionProvider(*opts)
		So(err, ShouldBeNil)

		export := BongoExport{
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
