package command

type Command interface {
	Diff(Command) (Diff, error)
}

type Diff interface {
	ToRows() [][]string
}
