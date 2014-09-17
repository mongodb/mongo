package command

// Interface for a single command that can be run against a MongoDB connection.
type Command interface {

	// Convert to an interface that can be passed to the Run() method of
	// a mgo.DB instance
	AsRunnable() interface{}

	// Diff the Command against another Command
	Diff(Command) (Diff, error)
}

// Interface for a diff between the results of two commands run against the
// database.
type Diff interface {

	// Convert to rows, to be printed easily.
	ToRows() [][]string
}
