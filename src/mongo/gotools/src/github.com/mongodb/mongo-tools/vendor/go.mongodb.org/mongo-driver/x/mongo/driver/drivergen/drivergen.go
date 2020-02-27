package drivergen

import (
	"bytes"
	"fmt"
	"io"
	"sort"
	"strings"
	"text/template"
	"unicode"

	"github.com/pelletier/go-toml"
)

// Operation is the top-level configuration type. It's the direct representation of an operation
// TOML file.
type Operation struct {
	Name           string
	Documentation  string
	Version        int
	DriverInternal bool
	Properties     Properties
	Command        Command
	Request        map[string]RequestField
	Response       Response

	pkg string
}

// PackageName returns the package name to use when generating the operation.
func (op Operation) PackageName() string { return op.pkg }

// Generate creates the operation type and associated response types and writes them to w.
func (op Operation) Generate(w io.Writer) error {
	t, err := template.New(op.Name + " operation").Parse(typeTemplate)
	if err != nil {
		return err
	}
	return t.Execute(w, op)
}

// ShortName returns the receiver used for this operation.
func (op Operation) ShortName() string {
	name := op.Name
	if len(name) == 0 {
		return ""
	}
	short := strings.ToLower(string(name[0]))
	idx := 1
	for {
		i := strings.IndexFunc(name[idx:], unicode.IsUpper)
		if i == -1 {
			break
		}
		idx += i
		short += strings.ToLower(string(name[idx]))
		idx++
	}
	return short
}

// ResultType returns the type to use as the result of running this operation.
func (op Operation) ResultType() string {
	return op.Response.Name
}

// Title wraps strings.Title for use in templates.
func (op Operation) Title(name string) string { return strings.Title(name) }

// EscapeDocumentation will add the required // in front of each line of documentation.
func (Operation) EscapeDocumentation(doc string) string {
	var slc []string
	for _, line := range strings.Split(doc, "\n") {
		slc = append(slc, "// "+line)
	}
	return strings.Join(slc, "\n")
}

// ConstructorParameters builds the parameter names and types for the operation constructor.
func (op Operation) ConstructorParameters() string {
	var parameters []string
	for name, field := range op.Request {
		if !field.Constructor {
			continue
		}
		parameters = append(parameters, name+" "+field.ParameterType())
	}
	return strings.Join(parameters, ", ")
}

// ConstructorFields returns a slice of name name pairs that set fields in a newly instantiated
// operation.
func (op Operation) ConstructorFields() []string {
	var fields []string
	for name, field := range op.Request {
		if !field.Constructor {
			continue
		}
		// either "name: name," or "name: &name,"
		fieldName := name
		if field.PointerType() {
			fieldName = "&" + name
		}
		fields = append(fields, name+": "+fieldName+",")
	}
	return fields
}

// CommandMethod returns the code required to transform the operation into a command. This code only
// returns the contents of the command method, without the function definition and return.
func (op Operation) CommandMethod() (string, error) {
	var buf bytes.Buffer
	switch op.Command.Parameter {
	case "collection":
		tmpl := commandCollectionTmpl
		if op.Command.Database {
			tmpl = commandCollectionDatabaseTmpl
		}
		err := tmpl.Execute(&buf, op)
		if err != nil {
			return "", err
		}
	case "database":
		tmpl := commandDatabaseTmpl
		err := tmpl.Execute(&buf, op)
		if err != nil {
			return "", err
		}
	default:
		var tmpl *template.Template
		field, ok := op.Request[op.Command.Parameter]
		if !ok {
			return "", fmt.Errorf(
				"no request field named '%s' but '%s' is specified as the command parameter",
				op.Command.Parameter, op.Command.Parameter,
			)
		}
		switch field.Type {
		case "double":
			tmpl = commandParamDoubleTmpl
		case "string":
			tmpl = commandParamStringTmpl
		case "document":
			tmpl = commandParamDocumentTmpl
		case "array":
			tmpl = commandParamArrayTmpl
		case "boolean":
			tmpl = commandParamBooleanTmpl
		case "int32":
			tmpl = commandParamInt32Tmpl
		case "int64":
			tmpl = commandParamInt64Tmpl
		default:
			return "", fmt.Errorf("unknown request field type %s", field.Type)
		}
		var rf struct {
			ShortName              string
			Name                   string
			ParameterName          string
			MinWireVersion         int
			MinWireVersionRequired int
		}
		rf.ShortName = op.ShortName()
		rf.Name = op.Command.Parameter
		rf.ParameterName = op.Command.Name
		rf.MinWireVersion = field.MinWireVersion
		rf.MinWireVersionRequired = field.MinWireVersionRequired
		err := tmpl.Execute(&buf, rf)
		if err != nil {
			return "", err
		}
	}
	names := make([]string, 0, len(op.Request))
	for name := range op.Request {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		field := op.Request[name]
		if name == op.Properties.Batches || field.Skip {
			continue
		}
		var tmpl *template.Template
		switch field.Type {
		case "double":
			tmpl = commandParamDoubleTmpl
		case "string":
			tmpl = commandParamStringTmpl
		case "document":
			tmpl = commandParamDocumentTmpl
		case "array":
			tmpl = commandParamArrayTmpl
		case "boolean":
			tmpl = commandParamBooleanTmpl
		case "int32":
			tmpl = commandParamInt32Tmpl
		case "int64":
			tmpl = commandParamInt64Tmpl
		case "value":
			tmpl = commandParamValueTmpl
		default:
			return "", fmt.Errorf("unknown request field type %s", field.Type)
		}
		var rf struct {
			ShortName              string
			Name                   string
			ParameterName          string
			MinWireVersion         int
			MinWireVersionRequired int
		}
		rf.ShortName = op.ShortName()
		rf.Name = name
		rf.ParameterName = name
		if field.KeyName != "" {
			rf.ParameterName = field.KeyName
		}
		rf.MinWireVersion = field.MinWireVersion
		rf.MinWireVersionRequired = field.MinWireVersionRequired
		err := tmpl.Execute(&buf, rf)
		if err != nil {
			return "", err
		}

	}
	return buf.String(), nil
}

// Properties represent general properties of the operation.
type Properties struct {
	Disabled                       []Builtin
	Enabled                        []Builtin
	Retryable                      Retryable
	Batches                        string
	Legacy                         LegacyOperation
	MinimumWriteConcernWireVersion int
	MinimumReadConcernWireVersion  int
}

// Builtins returns a slice of built-ins that is the combination of the non-disabled default
// built-ins plus any enabled non-default built-ins.
func (p Properties) Builtins() []Builtin {
	defaults := map[Builtin]struct{}{
		Deployment:     {},
		Database:       {},
		Selector:       {},
		CommandMonitor: {},
		ClientSession:  {},
		ClusterClock:   {},
		Collection:     {},
		Crypt:          {},
	}
	for _, builtin := range p.Disabled {
		delete(defaults, builtin)
	}
	builtins := make([]Builtin, 0, len(defaults)+len(p.Enabled))
	// We don't do this in a loop because we want them to be in a stable order.
	if _, ok := defaults[Deployment]; ok {
		builtins = append(builtins, Deployment)
	}
	if _, ok := defaults[Database]; ok {
		builtins = append(builtins, Database)
	}
	if _, ok := defaults[Selector]; ok {
		builtins = append(builtins, Selector)
	}
	if _, ok := defaults[CommandMonitor]; ok {
		builtins = append(builtins, CommandMonitor)
	}
	if _, ok := defaults[ClientSession]; ok {
		builtins = append(builtins, ClientSession)
	}
	if _, ok := defaults[ClusterClock]; ok {
		builtins = append(builtins, ClusterClock)
	}
	if _, ok := defaults[Collection]; ok {
		builtins = append(builtins, Collection)
	}
	if _, ok := defaults[Crypt]; ok {
		builtins = append(builtins, Crypt)
	}
	for _, builtin := range p.Enabled {
		switch builtin {
		case Deployment, Database, Selector, CommandMonitor, ClientSession, ClusterClock, Collection, Crypt:
			continue // If someone added a default to enable, just ignore it.
		}
		builtins = append(builtins, builtin)
	}
	sort.Slice(builtins, func(i, j int) bool { return builtins[i] < builtins[j] })
	return builtins
}

// ExecuteBuiltins returns the builtins that need to be set on the driver.Operation for the
// properties set.
func (p Properties) ExecuteBuiltins() []Builtin {
	builtins := p.Builtins()
	fields := make([]Builtin, 0, len(builtins))
	for _, builtin := range builtins {
		if builtin == Collection {
			continue // We don't include this in execute.
		}
		fields = append(fields, builtin)
	}
	return fields
}

// IsEnabled returns a Builtin if the string that matches that built-in is enabled. If it's not, an
// empty string is returned.
func (p Properties) IsEnabled(builtin string) Builtin {
	m := p.BuiltinsMap()
	if b := m[Builtin(builtin)]; b {
		return Builtin(builtin)
	}
	return ""
}

// BuiltinsMap returns a map with the builtins that enabled.
func (p Properties) BuiltinsMap() map[Builtin]bool {
	builtins := make(map[Builtin]bool)
	for _, builtin := range p.Builtins() {
		builtins[builtin] = true
	}
	return builtins
}

// LegacyOperationKind returns the corresponding LegacyOperationKind value for an operation.
func (p Properties) LegacyOperationKind() string {
	switch p.Legacy {
	case LegacyFind:
		return "driver.LegacyFind"
	case LegacyGetMore:
		return "driver.LegacyGetMore"
	case LegacyKillCursors:
		return "driver.LegacyKillCursors"
	case LegacyListCollections:
		return "driver.LegacyListCollections"
	case LegacyListIndexes:
		return "driver.LegacyListIndexes"
	default:
		return "driver.LegacyNone"
	}
}

// Retryable represents retryable information for an operation.
type Retryable struct {
	Mode RetryableMode
	Type RetryableType
}

// RetryableMode are the configuration representations of the retryability modes.
type RetryableMode string

// These constants are the various retryability modes.
const (
	RetryableOnce           RetryableMode = "once"
	RetryableOncePerCommand RetryableMode = "once per command"
	RetryableContext        RetryableMode = "context"
)

// RetryableType instances are the configuration representation of a kind of retryability.
type RetryableType string

// These constants are the various retryable types.
const (
	RetryableWrites RetryableType = "writes"
	RetryableReads  RetryableType = "reads"
)

// LegacyOperation enables legacy versions of find, getMore, or killCursors operations.
type LegacyOperation string

// These constants are the various legacy operations that can be generated.
const (
	LegacyFind            LegacyOperation = "find"
	LegacyGetMore         LegacyOperation = "getMore"
	LegacyKillCursors     LegacyOperation = "killCursors"
	LegacyListCollections LegacyOperation = "listCollections"
	LegacyListIndexes     LegacyOperation = "listIndexes"
)

// Builtin represent types that are built into the IDL.
type Builtin string

// These constants are the built in types.
const (
	Collection     Builtin = "collection"
	ReadPreference Builtin = "read preference"
	ReadConcern    Builtin = "read concern"
	WriteConcern   Builtin = "write concern"
	CommandMonitor Builtin = "command monitor"
	ClientSession  Builtin = "client session"
	ClusterClock   Builtin = "cluster clock"
	Selector       Builtin = "selector"
	Database       Builtin = "database"
	Deployment     Builtin = "deployment"
	Crypt          Builtin = "crypt"
)

// ExecuteName provides the name used when setting this built-in on a driver.Operation.
func (b Builtin) ExecuteName() string {
	var execname string
	switch b {
	case ReadPreference:
		execname = "ReadPreference"
	case ReadConcern:
		execname = "ReadConcern"
	case WriteConcern:
		execname = "WriteConcern"
	case CommandMonitor:
		execname = "CommandMonitor"
	case ClientSession:
		execname = "Client"
	case ClusterClock:
		execname = "Clock"
	case Selector:
		execname = "Selector"
	case Database:
		execname = "Database"
	case Deployment:
		execname = "Deployment"
	case Crypt:
		execname = "Crypt"
	}
	return execname
}

// ReferenceName returns the short name used to refer to this built in. It is used as the field name
// in the struct and as the variable name for the setter.
func (b Builtin) ReferenceName() string {
	var refname string
	switch b {
	case Collection:
		refname = "collection"
	case ReadPreference:
		refname = "readPreference"
	case ReadConcern:
		refname = "readConcern"
	case WriteConcern:
		refname = "writeConcern"
	case CommandMonitor:
		refname = "monitor"
	case ClientSession:
		refname = "session"
	case ClusterClock:
		refname = "clock"
	case Selector:
		refname = "selector"
	case Database:
		refname = "database"
	case Deployment:
		refname = "deployment"
	case Crypt:
		refname = "crypt"
	}
	return refname
}

// SetterName returns the name to be used when creating a setter for this built-in.
func (b Builtin) SetterName() string {
	var setter string
	switch b {
	case Collection:
		setter = "Collection"
	case ReadPreference:
		setter = "ReadPreference"
	case ReadConcern:
		setter = "ReadConcern"
	case WriteConcern:
		setter = "WriteConcern"
	case CommandMonitor:
		setter = "CommandMonitor"
	case ClientSession:
		setter = "Session"
	case ClusterClock:
		setter = "ClusterClock"
	case Selector:
		setter = "ServerSelector"
	case Database:
		setter = "Database"
	case Deployment:
		setter = "Deployment"
	case Crypt:
		setter = "Crypt"
	}
	return setter
}

// Type returns the Go type for this built-in.
func (b Builtin) Type() string {
	var t string
	switch b {
	case Collection:
		t = "string"
	case ReadPreference:
		t = "*readpref.ReadPref"
	case ReadConcern:
		t = "*readconcern.ReadConcern"
	case WriteConcern:
		t = "*writeconcern.WriteConcern"
	case CommandMonitor:
		t = "*event.CommandMonitor"
	case ClientSession:
		t = "*session.Client"
	case ClusterClock:
		t = "*session.ClusterClock"
	case Selector:
		t = "description.ServerSelector"
	case Database:
		t = "string"
	case Deployment:
		t = "driver.Deployment"
	case Crypt:
		t = "*driver.Crypt"
	}
	return t
}

// Documentation returns the GoDoc documentation for this built-in.
func (b Builtin) Documentation() string {
	var doc string
	switch b {
	case Collection:
		doc = "Collection sets the collection that this command will run against."
	case ReadPreference:
		doc = "ReadPreference set the read prefernce used with this operation."
	case ReadConcern:
		doc = "ReadConcern specifies the read concern for this operation."
	case WriteConcern:
		doc = "WriteConcern sets the write concern for this operation."
	case CommandMonitor:
		doc = "CommandMonitor sets the monitor to use for APM events."
	case ClientSession:
		doc = "Session sets the session for this operation."
	case ClusterClock:
		doc = "ClusterClock sets the cluster clock for this operation."
	case Selector:
		doc = "ServerSelector sets the selector used to retrieve a server."
	case Database:
		doc = "Database sets the database to run this operation against."
	case Deployment:
		doc = "Deployment sets the deployment to use for this operation."
	case Crypt:
		doc = "Crypt sets the Crypt object to use for automatic encryption and decryption."
	}
	return doc
}

// Command holds the command serialization specific information for an operation.
type Command struct {
	Name      string
	Parameter string
	Database  bool
}

// RequestField represents an individual operation field.
type RequestField struct {
	Type                   string
	Slice                  bool
	Constructor            bool
	Variadic               bool
	Skip                   bool
	Documentation          string
	MinWireVersion         int
	MinWireVersionRequired int
	KeyName                string
}

// Command returns a string function that sets the key to name and value to the RequestField type.
// It uses accessor to access the parameter. The accessor parameter should be the shortname of the
// operation and the name of the field of the property used for the command name. For example, if
// the shortname is "eo" and the field is "collection" then accessor should be "eo.collection".
func (rf RequestField) Command(name, accessor string) string {
	return ""
}

// ParameterType returns this field's type for use as a parameter argument.
func (rf RequestField) ParameterType() string {
	var param string
	if rf.Slice && !rf.Variadic {
		param = "[]"
	}
	if rf.Variadic {
		param = "..."
	}
	switch rf.Type {
	case "double":
		param += "float64"
	case "string":
		param += "string"
	case "document", "array":
		param += "bsoncore.Document"
	case "binary":
	case "boolean":
		param += "bool"
	case "int32":
		param += "int32"
	case "int64":
		param += "int64"
	case "value":
		param += "bsoncore.Value"
	}
	return param
}

// DeclarationType returns this field's type for use in a struct type declaration.
func (rf RequestField) DeclarationType() string {
	var decl string
	switch rf.Type {
	case "double", "string", "boolean", "int32", "int64":
		decl = "*"
	}
	if rf.Slice {
		decl = "[]"
	}
	switch rf.Type {
	case "double":
		decl += "float64"
	case "string":
		decl += "string"
	case "document", "array":
		decl += "bsoncore.Document"
	case "binary":
	case "boolean":
		decl += "bool"
	case "int32":
		decl += "int32"
	case "int64":
		decl += "int64"
	case "value":
		decl += "bsoncore.Value"
	}
	return decl
}

// PointerType returns true if the request field is a pointer type and the setter should take the
// address when setting via a setter method.
func (rf RequestField) PointerType() bool {
	switch rf.Type {
	case "double", "string", "boolean", "int32", "int64":
		return true
	default:
		return false
	}
}

// Response represents a response type to generate.
type Response struct {
	Name  string
	Type  string
	Field map[string]ResponseField
}

// ResponseField is an individual field of a response.
type ResponseField struct {
	Type          string
	Documentation string
}

// DeclarationType returns the field's type for use in a struct type declaration.
func (rf ResponseField) DeclarationType() string {
	switch rf.Type {
	case "boolean":
		return "bool"
	case "value":
		return "bsoncore.Value"
	default:
		return rf.Type
	}
}

// BuiltinResponseType is the type used to define built in response types.
type BuiltinResponseType string

// These constants represents the different built in response types.
const (
	BatchCursor BuiltinResponseType = "batch cursor"
)

// BuildMethod handles creating the body of a method to create a response from a BSON response
// document.
//
// TODO(GODRIVER-1094): This method is hacky because we're not using nested templates like we should
// be. Each template should be registered and we should be calling the template to create it.
func (r Response) BuildMethod() (string, error) {
	var buf bytes.Buffer
	names := make([]string, 0, len(r.Field))
	for name := range r.Field {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		field := r.Field[name]
		var tmpl *template.Template
		switch field.Type {
		case "boolean":
			tmpl = responseFieldBooleanTmpl
		case "int32":
			tmpl = responseFieldInt32Tmpl
		case "int64":
			tmpl = responseFieldInt64Tmpl
		case "string":
			tmpl = responseFieldStringTmpl
		case "value":
			tmpl = responseFieldValueTmpl
		case "document":
			tmpl = responseFieldDocumentTmpl
		default:
			return "", fmt.Errorf("unknown response field type %s", field.Type)
		}
		var rf struct {
			ResponseName      string // Key of the BSON response.
			ResponseShortName string // Receiver for the type being built.
			Field             string // Name of the Go response type field.
		}
		rf.ResponseShortName = r.ShortName()
		rf.ResponseName = name
		rf.Field = strings.Title(name)
		err := tmpl.Execute(&buf, rf)
		if err != nil {
			return "", err
		}

	}
	return buf.String(), nil
}

// ShortName returns the short name used when constructing a response.
func (r Response) ShortName() string {
	name := r.Name
	if len(name) == 0 {
		return ""
	}
	short := strings.ToLower(string(name[0]))
	idx := 1
	for {
		i := strings.IndexFunc(name[idx:], unicode.IsUpper)
		if i == -1 {
			break
		}
		idx += i
		short += strings.ToLower(string(name[idx]))
		idx++
	}
	return short
}

// ParseFile will construct an Operation using the TOML in filename. The Operation will have the
// package name set to packagename.
func ParseFile(filename, packagename string) (Operation, error) {
	tree, err := toml.LoadFile(filename)
	if err != nil {
		return Operation{}, err
	}
	var op Operation
	err = tree.Unmarshal(&op)
	op.pkg = packagename
	return op, err
}
