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

	"go.mongodb.org/mongo-driver/x/bsonx/bsoncore"
	"go.mongodb.org/mongo-driver/x/mongo/driver/connstring"
	"go.mongodb.org/mongo-driver/x/mongo/driver/description"
	"go.mongodb.org/mongo-driver/x/mongo/driver/operation"
	"go.mongodb.org/mongo-driver/x/mongo/driver/topology"
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
	err = t.Connect()
	if err != nil {
		log.Fatal(err)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	dbname := cs.Database
	if dbname == "" {
		dbname = "test"
	}

	op := operation.NewCommand(bsoncore.BuildDocument(nil, bsoncore.AppendStringElement(nil, "count", *col))).
		Deployment(t).Database(dbname).ServerSelector(description.WriteSelector())
	err = op.Execute(ctx)
	if err != nil {
		log.Fatalf("failed executing count command on %s.%s: %v", dbname, *col, err)
	}
	rdr := op.Result()
	log.Println(rdr)
}
