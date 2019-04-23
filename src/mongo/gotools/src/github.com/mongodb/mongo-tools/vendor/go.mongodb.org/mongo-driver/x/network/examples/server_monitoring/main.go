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

	"github.com/kr/pretty"
	"go.mongodb.org/mongo-driver/x/mongo/driver/topology"
	"go.mongodb.org/mongo-driver/x/network/address"
	"go.mongodb.org/mongo-driver/x/network/connection"
)

func main() {
	s, err := topology.ConnectServer(
		context.Background(),
		address.Address("localhost:27017"),
		topology.WithHeartbeatInterval(func(time.Duration) time.Duration { return 2 * time.Second }),
		topology.WithConnectionOptions(
			func(opts ...connection.Option) []connection.Option {
				return append(opts, connection.WithAppName(func(string) string { return "server monitoring test" }))
			},
		),
	)
	if err != nil {
		log.Fatalf("could not start server: %v", err)
	}

	sub, err := s.Subscribe()
	if err != nil {
		log.Fatalf("could not subscribe to server: %v", err)
	}

	for desc := range sub.C {
		log.Printf("%# v", pretty.Formatter(desc))
	}
}
