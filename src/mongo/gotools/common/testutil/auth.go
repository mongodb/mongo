// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package testutil

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/options"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"os"
)

var (
	UserAdmin              = "uAdmin"
	UserAdminPassword      = "password"
	CreatedUserNameEnv     = "AUTH_USERNAME"
	CreatedUserPasswordEnv = "AUTH_PASSWORD"
)

// Initialize a user admin, using the already-connected session passed in.
// Assumes that there are no existing users, otherwise will fail with a
// permissions issue.
func CreateUserAdmin(session *mgo.Session) error {
	err := CreateUserWithRole(session, UserAdmin, UserAdminPassword,
		mgo.RoleUserAdminAny, false)
	return err
}

// Create a user with the specified password and role, using the
// already-connected session passed in.  If needsLogin is true, then the
// default user admin and password will be used to log in to the admin
// db before creating the user.
func CreateUserWithRole(session *mgo.Session, user,
	password string, role mgo.Role, needsLogin bool) error {

	adminDB := session.DB("admin")
	if needsLogin {
		err := adminDB.Login(
			UserAdmin,
			UserAdminPassword,
		)
		if err != nil {
			return fmt.Errorf("error logging in: %v", err)
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
		return fmt.Errorf("error adding user %v with role %v: %v", user, role, err)
	}

	return nil
}

func GetAuthOptions() options.Auth {
	if HasTestType(AuthTestType) {
		return options.Auth{
			Username: os.Getenv(CreatedUserNameEnv),
			Password: os.Getenv(CreatedUserPasswordEnv),
			Source:   "admin",
		}
	}

	return options.Auth{}
}
