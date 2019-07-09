The `operationgen` tool
=======================
The `operationgen` tool is used to generate operations from the IDL accepted by the `drivergen`
package. Most of the documentation for code generation can be found in the `drivergen` package.

Building
--------
Building `operationgen` requires a special library called
[packr](https://github.com/gobuffalo/packr). Make sure you call `packr2 install` to install
`operationgen`. Before committing, ensure you call `packr2 clean` to remove the generated files.
This will help avoid bloating the git repository with unnecessary files.
