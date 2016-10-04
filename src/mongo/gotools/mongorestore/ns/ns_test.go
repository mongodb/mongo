package ns

import (
	"testing"

	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	. "github.com/smartystreets/goconvey/convey"
)

func init() {
	// bump up the verbosity to make checking debug log output possible
	log.SetVerbosity(&options.Verbosity{
		VLevel: 4,
	})
}

func TestEscape(t *testing.T) {
	Convey("with a few strings", t, func() {
		So(Escape("(blah)"), ShouldEqual, "(blah)")
		So(Escape(""), ShouldEqual, "")
		So(Escape(`bl*h*\\`), ShouldEqual, `bl\*h\*\\\\`)
		So(Escape("blah**"), ShouldEqual, `blah\*\*`)
	})
}

func TestUnescape(t *testing.T) {
	Convey("with a few escaped strings", t, func() {
		So(Unescape("(blah)"), ShouldEqual, "(blah)")
		So(Unescape(""), ShouldEqual, "")
		So(Unescape(`bl\*h\*\\\\`), ShouldEqual, `bl*h*\\`)
		So(Unescape(`blah\*\*`), ShouldEqual, "blah**")
	})
}

func TestReplacer(t *testing.T) {
	Convey("with replacements", t, func() {
		Convey(`'$db$.user$$' -> 'test.user$$_$db$', 'pr\*d\.*' -> 'st\*g\\ing.*'`, func() {
			r, err := NewRenamer([]string{"$db$.user$$", `pr\*d\\.*`}, []string{"test.user$$_$db$", `st\*g\\ing.*`})
			So(r, ShouldNotBeNil)
			So(err, ShouldBeNil)
			So(r.Get("stuff.user"), ShouldEqual, "test.user_stuff")
			So(r.Get("stuff.users"), ShouldEqual, "test.users_stuff")
			So(r.Get(`pr*d\.users`), ShouldEqual, `st*g\ing.users`)
			So(r.Get(`pr*d\.turbo.encabulators`), ShouldEqual, `st*g\ing.turbo.encabulators`)
			So(r.Get(`st*g\ing.turbo.encabulators`), ShouldEqual, `st*g\ing.turbo.encabulators`)
		})
		Convey(`'$:)*$.us(?:2)er$?$' -> 'test.us(?:2)er$?$_$:)*$'`, func() {
			r, err := NewRenamer([]string{"$:)*$.us(?:2)er$?$"}, []string{"test.us(?:2)er$?$_$:)*$"})
			So(r, ShouldNotBeNil)
			So(err, ShouldBeNil)
			So(r.Get("stuff.us(?:2)er"), ShouldEqual, "test.us(?:2)er_stuff")
			So(r.Get("stuff.us(?:2)ers"), ShouldEqual, "test.us(?:2)ers_stuff")
		})
		Convey("'*.*' -> '*_test.*'", func() {
			r, err := NewRenamer([]string{"*.*"}, []string{"*_test.*"})
			So(r, ShouldNotBeNil)
			So(err, ShouldBeNil)
			So(r.Get("stuff.user"), ShouldEqual, "stuff_test.user")
			So(r.Get("stuff.users"), ShouldEqual, "stuff_test.users")
			So(r.Get("prod.turbo.encabulators"), ShouldEqual, "prod_test.turbo.encabulators")
		})
	})
	Convey("with invalid replacements", t, func() {
		Convey("'$db$.user$db$' -> 'test.user-$db$'", func() {
			_, err := NewRenamer([]string{"$db$.user$db$"}, []string{"test.user-$db$"})
			So(err, ShouldNotBeNil)
		})
		Convey("'$db$.us$er$table$' -> 'test.user$table$_$db$'", func() {
			_, err := NewRenamer([]string{"$db$.us$er$table$"}, []string{"test.user$table$_$db$"})
			So(err, ShouldNotBeNil)
		})
	})
}

func TestMatcher(t *testing.T) {
	Convey("with matcher", t, func() {
		Convey(`'*.user*', 'pr\*d\.*'`, func() {
			m, err := NewMatcher([]string{`*.user*`, `pr\*d\.*`})
			So(m, ShouldNotBeNil)
			So(err, ShouldBeNil)
			So(m.Has("stuff.user"), ShouldBeTrue)
			So(m.Has("stuff.users"), ShouldBeTrue)
			So(m.Has("pr*d.users"), ShouldBeTrue)
			So(m.Has("pr*d.magic"), ShouldBeTrue)
			So(m.Has(`pr*d\.magic`), ShouldBeFalse)
			So(m.Has("prod.magic"), ShouldBeFalse)
			So(m.Has("pr*d.turbo.encabulators"), ShouldBeTrue)
			So(m.Has("st*ging.turbo.encabulators"), ShouldBeFalse)
		})
		Convey("'*.*'", func() {
			m, err := NewMatcher([]string{"*.*"})
			So(m, ShouldNotBeNil)
			So(err, ShouldBeNil)
			So(m.Has("stuff"), ShouldBeFalse)
			So(m.Has("stuff.user"), ShouldBeTrue)
			So(m.Has("stuff.users"), ShouldBeTrue)
			So(m.Has("prod.turbo.encabulators"), ShouldBeTrue)
		})
	})
	Convey("with invalid matcher", t, func() {
		Convey("'$.user$'", func() {
			_, err := NewMatcher([]string{"$.user$"})
			So(err, ShouldNotBeNil)
		})
		Convey("'*.user$'", func() {
			_, err := NewMatcher([]string{"*.user$"})
			So(err, ShouldNotBeNil)
		})
	})
}
