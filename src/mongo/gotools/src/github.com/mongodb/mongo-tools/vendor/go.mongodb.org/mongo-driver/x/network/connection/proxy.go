// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package connection

import "go.mongodb.org/mongo-driver/x/network/wiremessage"

// Proxy implements a MongoDB proxy. It will use the given pool to connect to a
// MongoDB server and proxy the traffic between connections it is given and the
// server. It will pass each of the wireops it reads from the handled connection
// to a Processor. If an error is returned from the processor, the wireop will
// not be forwarded onto the server. If there is not an error the returned message
// will be passed onto the server. If both the return message and the error are nil,
// the original wiremessage will be passed onto the server.
//
// TODO(GODRIVER-268): Implement this.
type Proxy struct {
	Processor wiremessage.Transformer
	Pool      Pool
}

// HandleConnection implements the Handler interface.
func (*Proxy) HandleConnection(Connection) { return }
