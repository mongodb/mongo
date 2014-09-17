package mongoexport

import (
	"encoding/json"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
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
		out, err := bsonutil.ConvertBSONValueToJSON(x)
		So(err, ShouldBeNil)

		jsonEncoder := json.NewEncoder(os.Stdout)
		jsonEncoder.Encode(out)
	})
}
