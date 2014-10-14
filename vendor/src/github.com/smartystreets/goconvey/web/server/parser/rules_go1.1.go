// +build go1.1,!go1.2

package parser

import "strings"

func noGoFiles(line string) bool {
	return strings.HasPrefix(line, "can't load package: ") &&
		strings.Contains(line, ": no Go source files in ")
}
func buildFailed(line string) bool {
	return strings.HasPrefix(line, "# ") ||
		strings.Contains(line, "cannot find package") ||
		(strings.HasPrefix(line, "can't load package: ") && !strings.Contains(line, ": no Go source files in "))
}
func noTestFunctions(line string) bool {
	return line == "testing: warning: no tests to run"
}
func noTestFiles(line string) bool {
	return strings.HasPrefix(line, "?") && strings.Contains(line, "[no test files]")
}
func isNewTest(line string) bool {
	return strings.HasPrefix(line, "=== ")
}
func isTestResult(line string) bool {
	return strings.HasPrefix(line, "--- ")
}
func isPackageReport(line string) bool {
	return (strings.HasPrefix(line, "FAIL") ||
		strings.HasPrefix(line, "exit status") ||
		strings.HasPrefix(line, "PASS") ||
		strings.HasPrefix(line, "ok  \t"))
}

func packageFailed(line string) bool {
	return strings.HasPrefix(line, "FAIL\t")
}
func packagePassed(line string) bool {
	return strings.HasPrefix(line, "ok  \t")
}
func isCoverageSummary(line string) bool {
	return false
}
