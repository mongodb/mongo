package options

type BSONDumpOptions struct {
	Type       string `long:"type" default:"json" description:"type of output: json, debug"`
	ObjCheck   bool   `long:"objcheck" description:"validate bson during processing"`
	NoObjCheck bool   `long:"noobjcheck" description:"don't validate bson during processing"`
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
