package mongoexport

import (
	"encoding/json"
	"github.com/shelman/mongo-tools-proto/common/bson_ext"
	. "github.com/smartystreets/goconvey/convey"
	"labix.org/v2/mgo/bson"
	"os"
	"testing"
)

func TestExtendedJSON(t *testing.T) {
	Convey("Serializing a doc to extended JSON should work", t, func() {
		x := bson.M{
			"_id": bson.NewObjectId(),
			"hey": "sup",
			"subdoc": bson.M{
				"subid": bson.NewObjectId(),
			},
			"array": []interface{}{
				bson.NewObjectId(),
				bson.Undefined,
			},
		}
		out := bson_ext.GetExtendedBSON(x)
		jsonEncoder := json.NewEncoder(os.Stdout)
		jsonEncoder.Encode(out)
	})
}
