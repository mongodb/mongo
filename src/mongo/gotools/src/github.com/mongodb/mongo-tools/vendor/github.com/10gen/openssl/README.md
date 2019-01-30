# OpenSSL bindings for Go

Please see http://godoc.org/github.com/spacemonkeygo/openssl for more info

### License

Copyright (C) 2017. See AUTHORS.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

### Installing on a Unix-ish system with pkg-config

1.  (If necessary) install the openssl C library with a package manager
    that provides an openssl.pc file OR install openssl manually and create
    an openssl.pc file.

2.  Ensure that `pkg-config --cflags --libs openssl` finds your openssl
    library.  If it doesn't, try setting `PKG_CONFIG_PATH` to the directory
    containing your openssl.pc file.  E.g. for darwin: with MacPorts,
    `PKG_CONFIG_PATH=/opt/local/lib/pkgconfig` or for Homebrew,
    `PKG_CONFIG_PATH=/usr/local/Cellar/openssl/1.0.2l/lib/pkgconfig`

### Installing on a Unix-ish system without pkg-config

1.  (If necessary) install the openssl C library in your customary way

2.  Set the `CGO_CPP_FLAGS`, `CGO_CFLAGS` and `CGO_LDFLAGS` as necessary to
    provide `-I`, `-L` and other options to the compiler.  E.g. on darwin,
    MongoDB's darwin build servers use the native libssl, but provide the
    missing headers in a custom directory, so it the build hosts set
    `CGO_CPPFLAGS=-I/opt/mongodbtoolchain/v2/include`

### Installing on Windows

1. Install [mingw-w64](http://mingw-w64.sourceforge.net/) and add it to
   your `PATH`

2. Install the C openssl into `C:\openssl`.  (Unfortunately, this is still
   hard-coded.)  You should have directories like `C:\openssl\include` and
   `C:\openssl\bin`.
