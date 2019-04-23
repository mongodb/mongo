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
	"math/rand"
	"net/http"
	_ "net/http/pprof"
	"os"
	"os/signal"
	"time"

	"go.mongodb.org/mongo-driver/mongo/options"
	"go.mongodb.org/mongo-driver/x/bsonx"

	"go.mongodb.org/mongo-driver/bson"

	"go.mongodb.org/mongo-driver/mongo/readpref"
	"go.mongodb.org/mongo-driver/x/mongo/driver"
	"go.mongodb.org/mongo-driver/x/mongo/driver/session"
	"go.mongodb.org/mongo-driver/x/mongo/driver/topology"
	"go.mongodb.org/mongo-driver/x/mongo/driver/uuid"
	"go.mongodb.org/mongo-driver/x/network/command"
	"go.mongodb.org/mongo-driver/x/network/description"
)

var concurrency = flag.Int("concurrency", 24, "how much concurrency should be used")
var ns = flag.String("namespace", "test.foo", "the namespace to use for test data")

func main() {

	go func() {
		log.Println(http.ListenAndServe("localhost:6060", nil))
	}()

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt)

	c, err := topology.New()

	if err != nil {
		log.Fatalf("unable to create topology: %s", err)
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

	var docs = make([]bsonx.Doc, 0, 1000)
	for i := 0; i < 1000; i++ {
		docs = append(docs, bsonx.Doc{{"_id", bsonx.Int32(int32(i))}})
	}

	ns := command.ParseNamespace(*ns)

	s, err := c.SelectServer(ctx, description.WriteSelector())
	if err != nil {
		return err
	}

	conn, err := s.Connection(ctx)
	if err != nil {
		return err
	}
	defer conn.Close()

	deletes := []bsonx.Doc{{
		{"q", bsonx.Document(bsonx.Doc{})},
		{"limit", bsonx.Int32(0)},
	}}
	_, err = (&command.Delete{WriteConcern: nil, NS: ns, Deletes: deletes}).RoundTrip(ctx, s.Description(), conn)
	if err != nil {
		return err
	}

	_, err = (&command.Insert{
		NS:   ns,
		Docs: docs,
	}).RoundTrip(
		ctx,
		s.Description(),
		conn,
	)

	return err
}

func work(ctx context.Context, idx int, c *topology.Topology) {
	r := rand.New(rand.NewSource(time.Now().Unix()))
	ns := command.ParseNamespace(*ns)
	rp := readpref.Nearest()
	for {
		select {
		case <-ctx.Done():
		default:

			limit := r.Intn(999) + 1

			pipeline := bsonx.Arr{bsonx.Document(bsonx.Doc{{"$limit", bsonx.Int32(int32(limit))}})}

			id, _ := uuid.New()
			aggOpts := options.Aggregate().SetBatchSize(200)
			cmd := command.Aggregate{
				NS:       ns,
				Pipeline: pipeline,
				ReadPref: rp,
			}
			cursor, err := driver.Aggregate(
				ctx, cmd, c,
				description.ReadPrefSelector(rp),
				description.ReadPrefSelector(rp),
				id,
				&session.Pool{},
				bson.DefaultRegistry,
				aggOpts,
			)
			if err != nil {
				log.Printf("%d-failed executing aggregate: %s", idx, err)
				continue
			}

			count := 0
			for cursor.Next(ctx) {
				count++
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
