package testutil

import (
	"labix.org/v2/mgo"
	"labix.org/v2/mgo/bson"
	"testing"
)

var (
	UserAdmin         = "uAdmin"
	UserAdminPassword = "password"
)

// Initialize a user admin, using the already-connected session passed in.
// Assumes that there are no existing users, otherwise will fail with a
// permissions issue.
func CreateUserAdmin(t *testing.T, session *mgo.Session) {
	CreateUserWithRole(t, session, UserAdmin, UserAdminPassword,
		mgo.RoleUserAdminAny, false)
}

// Create a user with the specified password and role, using the
// already-connected session passed in.  If needsLogin is true, then the
// default user admin and password will be used to log in to the admin
// db before creating the user.
func CreateUserWithRole(t *testing.T, session *mgo.Session, user,
	password string, role mgo.Role, needsLogin bool) {

	adminDB := session.DB("admin")
	if needsLogin {
		err := adminDB.Login(
			UserAdmin,
			UserAdminPassword,
		)
		if err != nil {
			t.Fatalf("error logging in: %v", err)
		}
	}

	err := adminDB.Run(
		bson.D{
			{"createUser", user},
			{"pwd", password},
			{"roles", []bson.M{
				bson.M{
					"role": role,
					"db":   "admin",
				},
			}},
		},
		&bson.M{},
	)

	if err != nil {
		t.Fatalf("error adding user %v with role %v: %v", user, role, err)
	}

}
