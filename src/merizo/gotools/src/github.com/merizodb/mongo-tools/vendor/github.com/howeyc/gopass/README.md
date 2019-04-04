# getpasswd in Go [![GoDoc](https://godoc.org/github.com/howeyc/gopass?status.svg)](https://godoc.org/github.com/howeyc/gopass)

Retrieve password from user terminal input without echo

Verified on BSD, Linux, and Windows.

Example:
```go
package main

import "fmt"
import "github.com/howeyc/gopass"

func main() {
	fmt.Printf("Password: ")
	pass := gopass.GetPasswd() // Silent, for *'s use gopass.GetPasswdMasked()
    // Do something with pass
}
```

Caution: Multi-byte characters not supported!
