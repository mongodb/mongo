// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package auth

import (
	"context"

	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/network/command"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// MongoDBX509 is the mechanism name for MongoDBX509.
const MongoDBX509 = "MONGODB-X509"

func newMongoDBX509Authenticator(cred *Cred) (Authenticator, error) {
	return &MongoDBX509Authenticator{User: cred.Username}, nil
}

// MongoDBX509Authenticator uses X.509 certificates over TLS to authenticate a connection.
type MongoDBX509Authenticator struct {
	User string
}

// Auth implements the Authenticator interface.
func (a *MongoDBX509Authenticator) Auth(ctx context.Context, desc description.Server, rw wiremessage.ReadWriter) error {
	authRequestDoc := bsonx.Doc{
		{"authenticate", bsonx.Int32(1)},
		{"mechanism", bsonx.String(MongoDBX509)},
	}

	if desc.WireVersion.Max < 5 {
		authRequestDoc = append(authRequestDoc, bsonx.Elem{"user", bsonx.String(a.User)})
	}

	authCmd := command.Read{DB: "$external", Command: authRequestDoc}
	ssdesc := description.SelectedServer{Server: desc}
	_, err := authCmd.RoundTrip(ctx, ssdesc, rw)
	if err != nil {
		return newAuthError("round trip error", err)
	}

	return nil
}
