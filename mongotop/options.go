package mongotop

// Output options for mongotop
type Output struct {
	Locks    bool `long:"locks" description:"report on use of per-database locks"`
	RowCount int  `long:"rowcount" short:"n" description:"number of stats lines to print (0 for indefinite)"`
	Json     bool `long:"json" description:"format output as JSON"`
}

func (_ *Output) Name() string {
	return "output"
}

func (_ *Output) PostParse() error {
	return nil
}

func (_ *Output) Validate() error {
	return nil
}
