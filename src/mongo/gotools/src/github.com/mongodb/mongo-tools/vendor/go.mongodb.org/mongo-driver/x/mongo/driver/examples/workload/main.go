// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"math/rand"
	"net/http"
	_ "net/http/pprof"
	"os"
	"os/signal"
	"strings"
	"time"

	"go.mongodb.org/mongo-driver/mongo/readpref"
	"go.mongodb.org/mongo-driver/x/bsonx/bsoncore"
	"go.mongodb.org/mongo-driver/x/mongo/driver"
	"go.mongodb.org/mongo-driver/x/mongo/driver/connstring"
	"go.mongodb.org/mongo-driver/x/mongo/driver/description"
	"go.mongodb.org/mongo-driver/x/mongo/driver/operation"
	"go.mongodb.org/mongo-driver/x/mongo/driver/topology"
)

var concurrency = flag.Int("concurrency", 24, "how much concurrency should be used")
var ns = flag.String("namespace", "test.foo", "the namespace to use for test data")

func main() {

	go func() {
		log.Println(http.ListenAndServe("localhost:6060", nil))
	}()

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt)

	cs, err := connstring.Parse("mongodb://localhost:27017/")
	if err != nil {
		log.Fatalf("unable to parse connection string: %v", err)
	}
	c, err := topology.New(topology.WithConnString(func(connstring.ConnString) connstring.ConnString {
		return cs
	}))
	if err != nil {
		log.Fatalf("unable to create topology: %s", err)
	}

	err = c.Connect()
	if err != nil {
		log.Fatalf("unable to connect topology: %s", err)
	}

	done := make(chan struct{})
	ctx, cancel := context.WithCancel(context.Background())
	go func() {
		<-sig
		cancel()
		close(done)
	}()

	log.Println("prepping")
	err = prep(ctx, c)
	if err != nil {
		log.Fatalf("unable to prep: %s", err)
	}
	log.Println("done prepping")

	log.Println("working")
	for i := 0; i < *concurrency; i++ {
		go work(ctx, i, c)
	}

	<-done
	log.Println("interrupt received: shutting down")
	_ = c.Disconnect(ctx)
	log.Println("finished")
}

func prep(ctx context.Context, c *topology.Topology) error {

	var docs = make([]bsoncore.Document, 0, 1000)
	for i := 0; i < 1000; i++ {
		docs = append(docs, bsoncore.BuildDocument(nil, bsoncore.AppendInt32Element(nil, "_id", int32(i))))
	}

	collection, database, err := parseNamespace(*ns)
	if err != nil {
		return err
	}

	deletes := bsoncore.BuildDocument(nil,
		bsoncore.BuildDocumentElement(nil, "q"),
		bsoncore.AppendInt32Element(nil, "limit", 0),
	)
	err = operation.NewDelete(deletes).Collection(collection).Database(database).
		Deployment(c).ServerSelector(description.WriteSelector()).Execute(ctx)
	if err != nil {
		return err
	}

	err = operation.NewInsert(docs...).Collection(collection).Database(database).Deployment(c).
		ServerSelector(description.WriteSelector()).Execute(ctx)
	return err
}

func work(ctx context.Context, idx int, c *topology.Topology) {
	r := rand.New(rand.NewSource(time.Now().Unix()))
	collection, database, err := parseNamespace(*ns)
	if err != nil {
		panic(fmt.Errorf("failed to parse namespace: %v", err))
	}
	rp := readpref.Nearest()
	for {
		select {
		case <-ctx.Done():
		default:

			limit := r.Intn(999) + 1

			pipeline := bsoncore.BuildArray(nil, bsoncore.BuildDocumentValue(bsoncore.AppendInt32Element(nil, "$limit", int32(limit))))

			op := operation.NewAggregate(pipeline).Collection(collection).Database(database).Deployment(c).BatchSize(200).
				ServerSelector(description.ReadPrefSelector(rp))
			err := op.Execute(ctx)
			if err != nil {
				log.Printf("%d-failed executing aggregate: %s", idx, err)
				continue
			}

			cursor, err := op.Result(driver.CursorOptions{BatchSize: 200})
			if err != nil {
				log.Printf("%d-failed to create cursor: %v", idx, err)
				continue
			}
			count := 0
			for cursor.Next(ctx) {
				count += cursor.Batch().DocumentCount()
			}
			if cursor.Err() != nil {
				_ = cursor.Close(ctx)
				log.Printf("%d-failed iterating aggregate results: %s", idx, cursor.Err())
				return
			}
			_ = cursor.Close(ctx)

			log.Printf("%d-iterated %d docs", idx, count)
		}
	}
}

func parseNamespace(ns string) (collection, database string, err error) {
	idx := strings.Index(ns, ".")
	if idx == -1 {
		return "", "", fmt.Errorf("invalid namespace: %s", ns)
	}
	return ns[idx+1:], ns[:idx], nil
}
