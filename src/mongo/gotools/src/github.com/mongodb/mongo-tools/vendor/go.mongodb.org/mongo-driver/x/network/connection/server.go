// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package connection

import "context"

// Server is used to handle incoming Connections. It handles the boilerplate of accepting a
// Connection and cleaning it up after running a Handler. This also makes it easier to build
// higher level processors, like proxies, by handling the life cycle of the underlying
// connection.
//
// TODO(GODRIVER-269): Implement this.
type Server struct {
	Addr    Addr
	Handler Handler
}

// ListenAndServe listens on the network address srv.Addr and calls Serve to
// handle requests on incoming connections. If srv.Addr is blank, "localhost:27017"
// is used.
func (*Server) ListenAndServe() error { return nil }

// Serve accepts incoming connections on the Listener l, creating a new service
// goroutine for each. The service goroutines call srv.Handler and do not processing
// beforehand. When srv.Handler returns, the connection is closed.
func (*Server) Serve(Listener) error { return nil }

// Shutdown gracefully shuts down the server by closing the active listeners. Shutdown
// does not handle or wait for all open connections to close and return before returning.
func (*Server) Shutdown(context.Context) error { return nil }

// Handler handles an individual Connection. Returning signals that the Connection
// is no longer needed and can be closed.
type Handler interface {
	HandleConnection(Connection)
}
