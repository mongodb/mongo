package bsondump

type BSONDumpOptions struct {
	Type     string `long:"type" default:"json" description:"type of output: debug, json"`
	ObjCheck bool   `long:"objcheck" description:"validate bson during processing"`
	Pretty bool `long:"pretty" description:"output to json in pretty mode"`
}

func (self *BSONDumpOptions) Name() string {
	return "output"
}

func (self *BSONDumpOptions) PostParse() error {
	return nil
}

func (self *BSONDumpOptions) Validate() error {
	return nil
}
