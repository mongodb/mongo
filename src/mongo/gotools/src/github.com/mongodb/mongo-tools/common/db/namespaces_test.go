package db

import (
	"fmt"
	"testing"

	. "github.com/smartystreets/goconvey/convey"
)

type stripDBFromNamespaceTestCase struct {
	inputNamespace string
	inputDBName    string

	outputNamespace string
	outputError     error
}

func TestStripDBFromNamespace(t *testing.T) {
	Convey("When testing StripDBFromNamespace with cases", t, func() {
		testCases := []stripDBFromNamespaceTestCase{
			{
				inputNamespace: "database.col",
				inputDBName:    "database",

				outputNamespace: "col",
				outputError:     nil,
			},
			{
				inputNamespace: "database2.col",
				inputDBName:    "database",

				outputNamespace: "",
				outputError:     fmt.Errorf("namespace 'database2.col' format is invalid - expected to start with 'database.'"),
			},
			{
				inputNamespace: "database.col",
				inputDBName:    "notAPrefix",

				outputNamespace: "",
				outputError:     fmt.Errorf("namespace 'database.col' format is invalid - expected to start with 'notAPrefix.'"),
			},
		}
		Convey("cases should match expected", func() {
			for _, tc := range testCases {
				resultNamespace, resultError := StripDBFromNamespace(tc.inputNamespace, tc.inputDBName)
				So(resultError, ShouldResemble, tc.outputError)
				So(resultNamespace, ShouldEqual, tc.outputNamespace)
			}
		})
	})

}
