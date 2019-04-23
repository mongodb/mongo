package main

import (
	"flag"
	"fmt"
	"html/template"
	"log"
	"os"
	"path/filepath"
	"strings"

	"golang.org/x/tools/go/packages"
)

var tmpl = `<!DOCTYPE html>
<html lang="en">
    <head>
        <meta http-equiv="refresh" content="5; url=https://godoc.org/go.mongodb.org/mongo-driver/{{.}}">
        <meta name=go-import content="go.mongodb.org/mongo-driver git https://github.com/mongodb/mongo-go-driver.git">
        <meta name="go-source" content="go.mongodb.org/mongo-driver https://github.com/mongodb/mongo-go-driver https://github.com/mongodb/mongo-go-driver/tree/master{/dir} https://github.com/mongodb/mongo-go-driver/blob/master{/dir}/{file}#L{line}">
    </head>
    <body>
        Redirecting to docs...
    </body>
</html>
`

func main() {
	var directory = "."
	var destination = "s3-website"
	fs := flag.NewFlagSet("", flag.ExitOnError)
	fs.Usage = func() {
		fmt.Fprintln(fs.Output(), "docbuilder is used to create the static site for the import paths for go.mongodb.org.")
		fmt.Fprintln(fs.Output(), "usage: docbuilder [flags] [destination]")
		fs.PrintDefaults()
	}
	fs.StringVar(&directory, "directory", ".", "directory to run docbuilder on")
	err := fs.Parse(os.Args[1:])
	if err == flag.ErrHelp {
		fs.Usage()
		os.Exit(0)
	}
	if err != nil {
		log.Fatalf("Could not parse flags: %v", err)
	}

	args := fs.Args()
	if len(args) > 0 {
		destination = args[0]
	}

	pkgs, err := packages.Load(&packages.Config{Dir: directory}, "./...")
	if err != nil {
		log.Fatalf("Could not load packages: %v", err)
	}

	dirs := make([]string, 1, len(pkgs)+1)
	for _, pkg := range pkgs {
		if !strings.HasPrefix(pkg.PkgPath, "go.mongodb.org") {
			continue
		}
		dirs = append(dirs, strings.TrimPrefix(pkg.PkgPath, "go.mongodb.org/mongo-driver"))
	}

	err = os.MkdirAll(filepath.Join(destination, "mongo-driver"), os.ModeDir|os.FileMode(0755))
	if err != nil {
		log.Fatalf("Could not make path: %v", err)
	}

	t, err := template.New("index").Parse(tmpl)
	if err != nil {
		log.Fatalf("Could not parse template: %v", err)
	}

	for _, dir := range dirs {
		directory := filepath.Join(destination, "mongo-driver", dir)
		err = os.MkdirAll(directory, os.ModeDir|os.FileMode(0755))
		if err != nil {
			log.Fatalf("Could not create directory (%s): %v", directory, err)
		}

		file, err := os.Create(filepath.Join(directory, "index.html"))
		if err != nil {
			log.Fatalf("Could not create index.html: %v", err)
		}

		err = t.Execute(file, dir)
		if err != nil {
			log.Fatalf("Could not execute template: %v", err)
		}
	}
}
