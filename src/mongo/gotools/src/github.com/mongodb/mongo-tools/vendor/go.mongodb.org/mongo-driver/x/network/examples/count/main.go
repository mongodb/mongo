// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package main

import (
	"context"
	"log"
	"time"

	"flag"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/session"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/topology"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/uuid"
	"go.mongodb.org/mongo-driver/x/network/command"
	"go.mongodb.org/mongo-driver/x/network/connstring"
	"go.mongodb.org/mongo-driver/x/network/description"
)

var uri = flag.String("uri", "mongodb://localhost:27017", "the mongodb uri to use")
var col = flag.String("c", "test", "the collection name to use")

func main() {

	flag.Parse()

	if *uri == "" {
		log.Fatalf("uri flag must have a value")
	}

	cs, err := connstring.Parse(*uri)
	if err != nil {
		log.Fatal(err)
	}

	t, err := topology.New(topology.WithConnString(func(connstring.ConnString) connstring.ConnString { return cs }))
	if err != nil {
		log.Fatal(err)
	}
	err = t.Connect(context.Background())
	if err != nil {
		log.Fatal(err)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	dbname := cs.Database
	if dbname == "" {
		dbname = "test"
	}

	id, _ := uuid.New()
	cmd := command.Read{DB: dbname, Command: bsonx.Doc{{"count", bsonx.String(*col)}}}
	rdr, err := driverlegacy.Read(
		ctx, cmd, t,
		description.WriteSelector(),
		id,
		&session.Pool{},
	)
	if err != nil {
		log.Fatalf("failed executing count command on %s.%s: %v", dbname, *col, err)
	}

	doc := bsonx.Doc{}
	err = doc.UnmarshalBSON(rdr)
	if err != nil {
		log.Fatal(err)
	}

	result, err := bson.MarshalExtJSON(doc, true, false)
	if err != nil {
		log.Fatalf("failed to convert BSON to extended JSON: %s", err)
	}
	log.Println(string(result))
}
