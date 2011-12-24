
def configure( env , fileLists , options ):
    #fileLists = { "serverOnlyFiles" : [] }

    myenv = env.Clone()
    if not options["windows"]:
        myenv.Append(CPPFLAGS=" -Wno-sign-compare -Wno-unused-function ") #snappy doesn't compile cleanly
    
    files = ["src/third_party/snappy/snappy.cc", "src/third_party/snappy/snappy-sinksource.cc"]

    fileLists["serverOnlyFiles"] += [ myenv.Object(f) for f in files ]

def configureSystem( env , fileLists , options ):
    configure( env , fileLists , options )
