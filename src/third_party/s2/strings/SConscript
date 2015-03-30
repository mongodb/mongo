# -*- mode: python -*-

Import("env")

env = env.Clone()

env.Append(CCFLAGS=['-Isrc/third_party/s2'])

env.Library(
    "strings",
    [ 
	"split.cc",
	"stringprintf.cc",
	"strutil.cc",
    ])
