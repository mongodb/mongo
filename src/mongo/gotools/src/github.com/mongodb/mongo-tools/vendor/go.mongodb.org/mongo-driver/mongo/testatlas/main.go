// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package main

import (
	"context"
	"flag"
	"log"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/mongo"
	"go.mongodb.org/mongo-driver/mongo/options"
)

func main() {
	flag.Parse()
	uris := flag.Args()

	for _, uri := range uris {
		ctx := context.Background()

		client, err := mongo.Connect(ctx, options.Client().ApplyURI(uri))
		if err != nil {
			log.Fatalf("failed creating and connecting to client: %v", err)
		}

		db := client.Database("test")
		defer func() {
			err := db.Drop(ctx)
			if err != nil {
				log.Fatalf("failed dropping database: %v", err)
			}

			err = client.Disconnect(ctx)
			if err != nil {
				log.Fatalf("failed disconnecting from client: %v", err)
			}
		}()

		coll := db.Collection("test")

		err = db.RunCommand(
			ctx,
			bson.D{{"isMaster", 1}},
		).Err()
		if err != nil {
			log.Fatalf("failed executing isMaster command: %v", err)
		}

		_, err = coll.InsertOne(ctx, bson.D{{"x", 1}})
		if err != nil {
			log.Fatalf("failed executing insertOne command: %v", err)
		}

		res := coll.FindOne(ctx, bson.D{{"x", 1}})
		if res.Err() != nil {
			log.Fatalf("failed executing findOne command: %v", res.Err())
		}
	}
}
