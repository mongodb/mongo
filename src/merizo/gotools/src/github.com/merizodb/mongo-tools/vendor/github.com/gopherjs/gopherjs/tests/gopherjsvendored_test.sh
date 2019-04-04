#!/bin/sh
# Don't run this file directly. It's executed as part of TestGopherJSCanBeVendored.

set -e

tmp=$(mktemp -d "${TMPDIR:-/tmp}/gopherjsvendored_test.XXXXXXXXXX")

cleanup() {
    rm -rf "$tmp"
    exit
}

trap cleanup EXIT HUP INT TERM

# copyGoPackage copies Go package with import path $1 to directory $2.
# The target directory is created if it doesn't exist.
copyGoPackage() {
    mkdir -p "$2"
    pkgDir=$(go list -f '{{.Dir}}' "$1")
    # Copy all files (not directories), other than ones that start with "." or "_".
    for f in $(find -H "$pkgDir" -maxdepth 1 -name "[^._]*" -type f); do
        cp "$f" "$2"
    done
}

# Make a hello project that will vendor GopherJS.
mkdir -p "$tmp/src/example.org/hello"
echo 'package main

import "github.com/gopherjs/gopherjs/js"

func main() {
    js.Global.Get("console").Call("log", "hello using js pkg")
}' > "$tmp/src/example.org/hello/main.go"

# Vendor GopherJS and its dependencies into hello project.
for pkg in $(go list -f '{{if not .Goroot}}{{.ImportPath}}{{end}}' $(go list -f '{{.ImportPath}} {{join .Deps " "}}' github.com/gopherjs/gopherjs)); do
    copyGoPackage "$pkg" "$tmp/src/example.org/hello/vendor/$pkg"
done

# Make $tmp our GOPATH workspace.
export GOPATH="$tmp"

# Build the vendored copy of GopherJS.
go install example.org/hello/vendor/github.com/gopherjs/gopherjs

# Use it to build and run the hello command.
(cd "$GOPATH/src/example.org/hello" && "$GOPATH/bin/gopherjs" run main.go)
