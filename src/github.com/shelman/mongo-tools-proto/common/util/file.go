package util

import (
	"bufio"
	"os"
)

// GetFieldsFromFile fetches the first line from the contents of the file
// at "path"
func GetFieldsFromFile(path string) ([]string, error) {
	fieldFileReader, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer fieldFileReader.Close()

	var fields []string
	fieldScanner := bufio.NewScanner(fieldFileReader)
	for fieldScanner.Scan() {
		fields = append(fields, fieldScanner.Text())
	}
	if err := fieldScanner.Err(); err != nil {
		return nil, err
	}
	return fields, nil
}
