package command

import ()

// Interface for a command to be run against the database.
type Command interface {

	// Produce an interface{} that can be passed to the Run() method of an
	// mgo.DB, in order to run the command against a MongoDB server.
	AsRunnable() interface{}
}
