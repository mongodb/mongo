// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package stat_consumer

import (
	"encoding/json"
	"fmt"

	"github.com/mongodb/mongo-tools/mongostat/stat_consumer/line"
)

// JSONLineFormatter converts the StatLines to JSON
type JSONLineFormatter struct {
	*limitableFormatter
}

func NewJSONLineFormatter(maxRows int64, _ bool) LineFormatter {
	return &JSONLineFormatter{
		limitableFormatter: &limitableFormatter{maxRows: maxRows},
	}
}

func init() {
	FormatterConstructors["json"] = NewJSONLineFormatter
}
func (glf *JSONLineFormatter) Finish() {
}

// FormatLines formats the StatLines as JSON
func (jlf *JSONLineFormatter) FormatLines(lines []*line.StatLine, headerKeys []string, keyNames map[string]string) string {
	// middle ground b/t the StatLines and the JSON string to be returned
	jsonFormat := map[string]interface{}{}

	// convert each StatLine to JSON
	for _, l := range lines {
		lineJson := make(map[string]interface{})

		if l.Printed && l.Error == nil {
			l.Error = fmt.Errorf("no data received")
		}
		l.Printed = true

		// check for error
		if l.Error != nil {
			lineJson["error"] = l.Error.Error()
			jsonFormat[l.Fields["host"]] = lineJson
			continue
		}

		for _, key := range headerKeys {
			lineJson[keyNames[key]] = l.Fields[key]
		}
		jsonFormat[l.Fields["host"]] = lineJson
	}

	// convert the JSON format of the lines to a json string to be returned
	linesAsJsonBytes, err := json.Marshal(jsonFormat)
	if err != nil {
		return fmt.Sprintf(`{"json error": "%v"}`, err.Error())
	}

	jlf.increment()
	return fmt.Sprintf("%s\n", linesAsJsonBytes)
}
