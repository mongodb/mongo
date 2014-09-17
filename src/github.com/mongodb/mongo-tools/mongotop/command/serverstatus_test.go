package command

import (
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestServerStatusCommandDiff(t *testing.T) {

	testutil.VerifyTestType(t, "unit")

	Convey("When diffing two ServerStatus commands", t, func() {

	})

}

func TestServerStatusDiffToRows(t *testing.T) {

	testutil.VerifyTestType(t, "unit")

	Convey("When converting a ServerStatusDiff to rows to be "+
		" printed", t, func() {

	})

}
