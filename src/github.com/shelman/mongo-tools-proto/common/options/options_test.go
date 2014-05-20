package options

import (
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

// mock version of extra options
type MockExtraOptions struct {
	RegisterCalled  bool
	PostParseCalled bool
	ValidateCalled  bool
	UsageCalled     bool
}

// funcs to satisfy the interface.  simply bookkeep that they have been called
func (self *MockExtraOptions) Register() {
	self.RegisterCalled = true
}

func (self *MockExtraOptions) PostParse() error {
	self.PostParseCalled = true
	return nil
}

func (self *MockExtraOptions) Validate() error {
	self.ValidateCalled = true
	return nil
}

func (self *MockExtraOptions) Usage() {
	self.UsageCalled = true
}

func TestExtraOptions(t *testing.T) {

	var mongoToolOptions *MongoToolOptions

	Convey("With an instance of MongoToolOptions", t, func() {

		mongoToolOptions = &MongoToolOptions{}

		Convey("when using extra options", func() {

			Convey("adding extra options should cause them to be"+
				" registered", func() {

				extra := &MockExtraOptions{}
				mongoToolOptions.AddOptions(extra)
				So(extra.RegisterCalled, ShouldBeTrue)

			})

			Convey("the parsing and validating functions for the extra options"+
				" should be called automatically by the top-level parsing and"+
				" validating", func() {

				extra := &MockExtraOptions{}
				mongoToolOptions.AddOptions(extra)
				mongoToolOptions.ParseAndValidate()
				So(extra.PostParseCalled, ShouldBeTrue)
				So(extra.ValidateCalled, ShouldBeTrue)

			})

		})

	})
}

func TestPostParse(t *testing.T) {

	var opts *MongoToolOptions

	Convey("With an instance of MongoToolOptions", t, func() {

		opts = &MongoToolOptions{}

		Convey("when executing the post-parsing step", func() {

			filterDB := "db"
			filterColl := "coll"

			Convey("specifying no filter should leave the filtering fields"+
				" blank", func() {

				So(opts.PostParse(), ShouldBeNil)
				So(opts.FilterNS, ShouldEqual, "")
				So(opts.FilterBoth, ShouldBeFalse)
				So(opts.FilterOnlyColl, ShouldBeFalse)

			})

			Convey("specifying only a db filter should lead to only the db"+
				" being set as a filter", func() {

				opts.DB = filterDB
				So(opts.PostParse(), ShouldBeNil)
				So(opts.FilterNS, ShouldEqual, filterDB+".")
				So(opts.FilterBoth, ShouldBeFalse)
				So(opts.FilterOnlyColl, ShouldBeFalse)

			})

			Convey("specifying only a collection filter should lead to only"+
				" the collection being set as a filter", func() {

				opts.Collection = filterColl
				So(opts.PostParse(), ShouldBeNil)
				So(opts.FilterNS, ShouldEqual, "."+filterColl)
				So(opts.FilterBoth, ShouldBeFalse)
				So(opts.FilterOnlyColl, ShouldBeTrue)

			})

			Convey("specifying both db and collection filters should lead to"+
				" the entire namespace being filtered", func() {

				opts.DB = filterDB
				opts.Collection = filterColl
				So(opts.PostParse(), ShouldBeNil)
				So(opts.FilterNS, ShouldEqual, filterDB+"."+filterColl)
				So(opts.FilterBoth, ShouldBeTrue)
				So(opts.FilterOnlyColl, ShouldBeFalse)

			})

		})

	})

}
