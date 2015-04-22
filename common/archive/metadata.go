package archive

import (
	"bytes"
	"github.com/mongodb/mongo-tools/common/intents"
)

type Metadata struct {
	*bytes.Buffer
	Intent *intents.Intent
}

func (md *Metadata) Open() error {
	return nil
}
func (md *Metadata) Close() error {
	return nil
}
