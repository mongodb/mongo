package options

type BSONDumpOptions struct {
	Filter string `long:"filter" description:"filter documents according to matcher expression"`
	Type   string `long:"type" default:"json" description:"type of output: json, debug"`
	//ObjCheck TODO ?
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
