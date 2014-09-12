package bsondump

import (
	"fmt"
	"github.com/shelman/mongo-tools-proto/bsondump/options"
	"github.com/shelman/mongo-tools-proto/common/bsonutil"
	"github.com/shelman/mongo-tools-proto/common/db"
	"github.com/shelman/mongo-tools-proto/common/json"
	commonopts "github.com/shelman/mongo-tools-proto/common/options"
	"gopkg.in/mgo.v2/bson"
	"io"
	"os"
)

type BSONDumper struct {
	ToolOptions     *commonopts.ToolOptions
	BSONDumpOptions *options.BSONDumpOptions
	FileName        string
	Out             io.Writer
}

func (bd *BSONDumper) ValidateSettings() error {
	//TODO
	return nil
}

func (bd *BSONDumper) init() (*db.BSONStream, error) {
	file, err := os.Open(bd.FileName)
	if err != nil {
		return nil, err
	}
	return db.NewBSONStream(file), nil
}

func dumpDoc(doc *bson.M, out io.Writer) error {
	extendedDoc, err := bsonutil.ConvertBSONValueToJSON(doc)
	if err != nil {
		return err
	}
	jsonBytes, err := json.Marshal(extendedDoc)
	if err != nil {
		return fmt.Errorf("Error converting BSON to extended JSON: %v", err)
	}
	_, err = out.Write(jsonBytes)
	return err
}

func (bd *BSONDumper) Dump() error {
	stream, err := bd.init()
	if err != nil {
		return err
	}

	decodedStream := db.NewDecodedBSONStream(stream)
	defer decodedStream.Close()

	var result bson.M
	for decodedStream.Next(&result) {
		if err := dumpDoc(&result, bd.Out); err != nil {
			return err
		}
		_, err := bd.Out.Write([]byte("\n"))
		if err != nil {
			return err
		}
	}
	if err := decodedStream.Err(); err != nil {
		return err
	}
	return nil
}
