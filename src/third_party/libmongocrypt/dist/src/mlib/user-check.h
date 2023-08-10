#ifndef MLIB_USER
#error                                                                                                                 \
    "The file being compiled transitively #include'd a mongo-mlib header, but is not a direct consumer of mlib, which is a private library for MongoDB C driver libraries"
#endif