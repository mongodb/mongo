package main

import (
	"bytes"
	"flag"
	"fmt"
	"log"
	"os"

	"go.mongodb.org/mongo-driver/x/mongo/driver/drivergen"
	"golang.org/x/tools/imports"
)

func main() {
	fs := flag.NewFlagSet("", flag.ExitOnError)
	fs.Usage = func() {
		fmt.Fprintln(fs.Output(), "operationgen is used to generate operation implementations.")
		fmt.Fprintln(fs.Output(), "usage: operationgen <operation configuration file> <package name> <generated file name>")
		fs.PrintDefaults()
	}
	var dryrun bool
	fs.BoolVar(&dryrun, "dryrun", false, "prints the output to stdout instead of writing to a file.")
	err := fs.Parse(os.Args[1:])
	if err == flag.ErrHelp {
		fs.Usage()
		os.Exit(0)
	}
	if err != nil {
		log.Fatalf("Could not parse flags: %v", err)
	}
	args := fs.Args()
	if len(args) < 3 {
		log.Println("Insufficient arguments specified.")
		fs.Usage()
		os.Exit(1)
	}
	config := args[0]
	pkg := args[1]
	filename := args[2]

	err = drivergen.Initialize()
	if err != nil {
		log.Fatalf("Could not initialize drivergen: %v", err)
	}

	op, err := drivergen.ParseFile(config, pkg)
	if err != nil {
		log.Fatalf("Could not parse configuration file '%s': %v", config, err)
	}
	var b bytes.Buffer
	err = op.Generate(&b)
	if err != nil {
		log.Fatalf("Could not generate operation: %v", err)
	}
	wd, err := os.Getwd()
	if err != nil {
		log.Fatalf("Could not get the current working directory: %v", err)
	}
	buf, err := imports.Process(wd, b.Bytes(), nil)
	if err != nil {
		if dryrun {
			fmt.Println(b.String())
		}
		log.Fatalf("Could not run goimports on generated file: %v", err)
	}
	if dryrun {
		os.Stdout.Write(buf)
		os.Exit(0)
	}

	file, err := os.Create(filename)
	if err != nil {
		log.Fatalf("Could not create %s: %v", filename, err)
	}

	_, err = file.Write(buf)
	if err != nil {
		log.Fatalf("Could not write to %s: %v", filename, err)
	}
}
