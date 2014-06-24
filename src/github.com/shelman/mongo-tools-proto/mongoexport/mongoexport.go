package mongoexport

import (
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/db"
	commonopts "github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/mongoexport/options"
	"io"
	"labix.org/v2/mgo/bson"
	"os"
	"strings"
	//"reflect"
	"time"
)

const (
	JSON_ARRAY_START = "["
	JSON_ARRAY_END   = "]"
)

// Wrapper for mongoexport functionality
type MongoExport struct {
	// generic mongo tool options
	ToolOptions *commonopts.ToolOptions

	OutputOpts *options.OutputFormatOptions

	InputOpts *options.InputOptions

	// for connecting to the db
	SessionProvider *db.SessionProvider
	ExportOutput    ExportOutput
}

func (exp *MongoExport) ValidateSettings() error {
	//TODO - on legacy mongoexport, if -d is blank, it assumes some default database.
	//Do we want to use that same behavior? It seems very odd to assume the DB
	//when only a collection is provided, but that's the behavior of the legacy tools.

	//Namespace must have a valid database and collection
	if exp.ToolOptions.Namespace.DB == "" || exp.ToolOptions.Namespace.Collection == "" {
		return fmt.Errorf("must specify a database and collection.")
	}
	return nil
}

func (exp *MongoExport) getOutputWriter() (io.Writer, error) {
	if exp.OutputOpts.OutputFile != "" {
		//TODO do we care if the file exists already? Overwrite it, or fail?
		file, err := os.Create(exp.OutputOpts.OutputFile)
		if err != nil {
			return nil, err
		}
		return file, err
	} else {
		return os.Stdout, nil
	}
}

func (exp *MongoExport) Export() (int64, error) {
	session, err := exp.SessionProvider.GetSession()
	if err != nil {
		return 0, err
	}

	out, err := exp.getOutputWriter()
	if err != nil {
		return 0, err
	}

	exportOutput, err := exp.getExportOutput(out)
	if err != nil {
		return 0, err
	}

	collection := session.DB(exp.ToolOptions.Namespace.DB).C(exp.ToolOptions.Namespace.Collection)

	//TODO get a real query
	query := bson.M{}
	cursor := collection.Find(query).Iter()
	defer cursor.Close()

	//Write headers
	err = exportOutput.WriteHeader()
	if err != nil {
		return 0, err
	}

	var result bson.M

	docsCount := int64(0)
	//Write document content
	for cursor.Next(&result) {
		err := exportOutput.ExportDocument(result)
		if err != nil {
			fmt.Println("err is !, ", err)
			return docsCount, err
		}
		docsCount++
	}

	//Write footers
	err = exportOutput.WriteFooter()
	if err != nil {
		return docsCount, err
	}

	return docsCount, nil
}

func (exp *MongoExport) getExportOutput(out io.Writer) (ExportOutput, error) {
	if !exp.OutputOpts.CSV {
		//TODO what if user specifies *both* --fields and --fieldFile?
		var fields []string
		var err error
		if len(exp.OutputOpts.Fields) > 0 {
			fields = strings.Split(exp.OutputOpts.Fields, ",")
		} else if exp.OutputOpts.FieldFile != "" {
			fields, err = getFieldsFromFile(exp.OutputOpts.FieldFile)
			if err != nil {
				return nil, err
			}
		}
		return NewCSVExportOutput(fields, out), nil
	} else {
		return NewJSONExportOutput(exp.OutputOpts.JSONArray, out), nil
	}

}

type ExportOutput interface {
	//WriteHeader outputs any pre-record headers that are written once
	//per output file.
	WriteHeader() error

	//WriteRecord writes the given document to the given io.Writer according to
	//the format supported by the underlying ExportOutput implementation.
	ExportDocument(bson.M) error

	//WriteFooter outputs any post-record headers that are written once per
	//output file.
	WriteFooter() error
}

func getExtendedJsonRepr(value interface{}) interface{} {
	switch t := value.(type) {
	case bson.M:
		for key, val := range t {
			t[key] = getExtendedJsonRepr(val)
		}
		return t
	case []interface{}:
		for index, val := range t {
			t[index] = getExtendedJsonRepr(val)
		}
		return t
	case int64:
		return NumberLongExt(t)
	case bson.ObjectId:
		return ObjectIdExt(t)
	case bson.MongoTimestamp:
		return MongoTimestampExt(t)
	case []byte:
		return GenericBinaryExt(t)
	case bson.Binary:
		return BinaryExt(t)
	case bson.JavaScript:
		return JavascriptExt(t)
	case time.Time:
		return TimeExt(t)
	case bson.RegEx:
		return RegExExt(t)
	default:
		return t
		//TODO
		// handle DBRefs
		// handle MinKey, MaxKey, and Undefined
	}
	return value
}
