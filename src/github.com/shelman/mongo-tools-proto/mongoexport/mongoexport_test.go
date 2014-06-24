package mongoexport

import (
	"encoding/json"
	//"fmt"
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
		//fmt.Println("hey")
		//encoder := json.NewEncoder(os.Stdout)
		//extendedJson(x, os.Stdout, encoder)
		out := getExtendedJsonRepr(x)
		//extendJson(&x)
		jsonEncoder := json.NewEncoder(os.Stdout)
		jsonEncoder.Encode(out)
	})
}
