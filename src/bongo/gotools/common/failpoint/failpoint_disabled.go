// +build !failpoints

package failpoint

func ParseFailpoints(_ string) {
}

func Get(fp string) (string, bool) {
	return "", false
}

func Enabled(fp string) bool {
	return false
}
