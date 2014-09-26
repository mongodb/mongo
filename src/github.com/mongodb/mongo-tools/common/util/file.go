package util

import (
	"bufio"
	"os"
	"path/filepath"
	"runtime"
	"strings"
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

func ToUniversalPath(unixPath string) string {
	if runtime.GOOS != "windows" {
		return unixPath
	}
	return strings.Replace(unixPath, "/", string(filepath.Separator), -1)
}
