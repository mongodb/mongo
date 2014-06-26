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

// Initialize a user admin for the specified server.  Assumes that there are
// no existing users, otherwise will fail with a permissions issue.
func CreateUserAdmin(t *testing.T, host, port string) {
	CreateUserWithRole(t, host, port, UserAdmin, UserAdminPassword,
		mgo.RoleUserAdminAny, false)
}

func CreateUserWithRole(t *testing.T, host, port, user, password string,
	role mgo.Role, needsLogin bool) {

	session, err := mgo.Dial(host + ":" + port)
	if err != nil {
		t.Fatalf("error dialing %v:%v - %v", host, port, err)
	}
	defer session.Close()

	adminDB := session.DB("admin")
	if needsLogin {
		err = adminDB.Login(
			UserAdmin,
			UserAdminPassword,
		)
		if err != nil {
			t.Fatalf("error logging in: %v", err)
		}
	}

	err = adminDB.Run(
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
