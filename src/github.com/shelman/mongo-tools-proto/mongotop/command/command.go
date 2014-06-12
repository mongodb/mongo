package command

type Command interface {
	AsRunnable() interface{}
	Diff(Command) (Diff, error)
}

type Diff interface {
	ToRows() [][]string
}
