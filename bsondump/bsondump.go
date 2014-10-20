package bsondump

import (
	"fmt"
	"github.com/mongodb/mongo-tools/bsondump/options"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/json"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"gopkg.in/mgo.v2/bson"
	"io"
	"os"
	"strings"
)

type BSONDump struct {
	ToolOptions     *commonopts.ToolOptions
	BSONDumpOptions *options.BSONDumpOptions
	FileName        string
	Out             io.Writer
}

func (bd *BSONDump) ValidateSettings() error {
	//TODO
	return nil
}

func (bd *BSONDump) init() (*db.BSONSource, error) {
	file, err := os.Open(bd.FileName)
	if err != nil {
		return nil, fmt.Errorf("Couldn't open BSON file: %v", err)
	}
	return db.NewBSONSource(file), nil
}

func dumpDoc(doc *bson.M, out io.Writer) error {
	extendedDoc, err := bsonutil.ConvertBSONValueToJSON(doc)
	if err != nil {
		return fmt.Errorf("Error converting BSON to extended JSON: %v", err)
	}
	jsonBytes, err := json.Marshal(extendedDoc)
	if err != nil {
		return fmt.Errorf("Error converting doc to JSON: %v", err)
	}
	_, err = out.Write(jsonBytes)
	return err
}

func (bd *BSONDump) Dump() error {
	stream, err := bd.init()
	if err != nil {
		return err
	}

	decodedStream := db.NewDecodedBSONSource(stream)
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

func (bd *BSONDump) Debug() error {
	stream, err := bd.init()
	if err != nil {
		return err
	}

	defer stream.Close()

	reusableBuf := make([]byte, db.MaxBSONSize)
	var result bson.Raw
	for {
		hasDoc, docSize := stream.LoadNextInto(reusableBuf)
		if !hasDoc {
			break
		}
		result.Data = reusableBuf[0:docSize]
		err = DebugBSON(result, 0, bd.Out)
		if err != nil {
			return err
		}
	}
	if err := stream.Err(); err != nil {
		return err
	}
	return nil
}

func DebugBSON(raw bson.Raw, indentLevel int, out io.Writer) error {
	indent := strings.Repeat("\t", indentLevel)
	fmt.Fprintf(out, "%v--- new object ---\n", indent)
	fmt.Fprintf(out, "%v\tsize : %v\n", indent, len(raw.Data))

	//Convert raw into an array of RawD we can iterate over.
	var rawD bson.RawD
	err := bson.Unmarshal(raw.Data, &rawD)
	if err != nil {
		return err
	}
	for _, rawElem := range rawD {
		fmt.Fprintf(out, "%v\t\t%v\n", indent, rawElem.Name)

		// the size of an element is the combined size of the following:
		// 1. 1 byte for the BSON type
		// 2. 'e_name' : the BSON key, which is a null-terminated cstring
		// 3. The BSON value
		// So size == 1 [size of type byte] +  1 [null byte for cstring key] + len(bson key) + len(bson value)
		// see http://bsonspec.org/spec.html for more details
		fmt.Fprintf(out, "%v\t\t\ttype: %4v size: %v\n", indent, rawElem.Value.Kind,
			2 + len(rawElem.Name) + len(rawElem.Value.Data) )

		//For nested objects or arrays, recurse.
		if rawElem.Value.Kind == 0x03 || rawElem.Value.Kind == 0x04 {
			err = DebugBSON(rawElem.Value, indentLevel+3, out)
			if err != nil {
				return err
			}
		}
	}
	return nil
}
