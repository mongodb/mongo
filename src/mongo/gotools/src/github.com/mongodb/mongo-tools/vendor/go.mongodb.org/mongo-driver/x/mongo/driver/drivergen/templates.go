package drivergen

import (
	"text/template"

	"github.com/gobuffalo/packr/v2"
)

// commandCollectionDatabaseTmpl is the template used to set the command name when the command can
// apply to either a collection or a database.
var commandCollectionDatabaseTmpl *template.Template

// commandCollectionTmpl is the template used to set the command name when the parameter will be a
// collection.
var commandCollectionTmpl *template.Template

// commandDatabaseTmpl is the template used to set the command name when the parameter will be a database.
var commandDatabaseTmpl *template.Template

var commandParamDocumentTmpl *template.Template
var commandParamArrayTmpl *template.Template
var commandParamValueTmpl *template.Template
var commandParamInt32Tmpl *template.Template
var commandParamInt64Tmpl *template.Template
var commandParamDoubleTmpl *template.Template
var commandParamBooleanTmpl *template.Template
var commandParamStringTmpl *template.Template

var responseFieldInt64Tmpl *template.Template
var responseFieldInt32Tmpl *template.Template
var responseFieldBooleanTmpl *template.Template
var responseFieldStringTmpl *template.Template
var responseFieldValueTmpl *template.Template
var responseFieldDocumentTmpl *template.Template

var typeTemplate string

var templates = packr.New("templates", "./templates")

// Initialize sets up drivergen for use. It must be called or all of the templates will be nil.
func Initialize() error {
	commandParameters, err := templates.FindString("command_parameter.tmpl")
	if err != nil {
		return err
	}
	commandParametersTmpl, err := template.New("commandParameters").Parse(commandParameters)
	if err != nil {
		return err
	}
	responseFields, err := templates.FindString("response_field.tmpl")
	if err != nil {
		return err
	}
	responseFieldsTmpl, err := template.New("responseFields").Parse(responseFields)
	if err != nil {
		return err
	}
	typeTemplate, err = templates.FindString("operation.tmpl")
	if err != nil {
		return err
	}

	commandCollectionDatabaseTmpl = commandParametersTmpl.Lookup("commandParamCollectionDatabase")
	commandCollectionTmpl = commandParametersTmpl.Lookup("commandParamCollection")
	commandDatabaseTmpl = commandParametersTmpl.Lookup("commandParamDatabase")

	commandParamDocumentTmpl = commandParametersTmpl.Lookup("commandParamDocument")
	commandParamArrayTmpl = commandParametersTmpl.Lookup("commandParamArray")
	commandParamBooleanTmpl = commandParametersTmpl.Lookup("commandParamBoolean")
	commandParamValueTmpl = commandParametersTmpl.Lookup("commandParamValue")
	commandParamInt32Tmpl = commandParametersTmpl.Lookup("commandParamInt32")
	commandParamInt64Tmpl = commandParametersTmpl.Lookup("commandParamInt64")
	commandParamDoubleTmpl = commandParametersTmpl.Lookup("commandParamDouble")
	commandParamStringTmpl = commandParametersTmpl.Lookup("commandParamString")

	responseFieldInt64Tmpl = responseFieldsTmpl.Lookup("responseFieldInt64")
	responseFieldInt32Tmpl = responseFieldsTmpl.Lookup("responseFieldInt32")
	responseFieldBooleanTmpl = responseFieldsTmpl.Lookup("responseFieldBoolean")
	responseFieldStringTmpl = responseFieldsTmpl.Lookup("responseFieldString")
	responseFieldValueTmpl = responseFieldsTmpl.Lookup("responseFieldValue")
	responseFieldDocumentTmpl = responseFieldsTmpl.Lookup("responseFieldDocument")

	return nil
}
