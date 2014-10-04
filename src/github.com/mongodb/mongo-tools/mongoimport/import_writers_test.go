package mongoimport

import (
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func init() {
	log.InitToolLogger(&options.Verbosity{
		Verbose: []bool{true, true, true, true},
	})
}

func TestInitImportShim(t *testing.T) {
	Convey("When initializing an import shim given an upsert flag...", t, func() {

	})
}
