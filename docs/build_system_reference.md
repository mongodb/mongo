# MongoDB Build System Reference

## MongoDB Build System Requirements
### Recommended minimum requirements
### Python modules
### External libraries
### Enterprise module requirements
### Testing requirements

## MongoDB customizations
### SCons modules
### Development tools
#### Compilation database generator
### Build tools
#### IDL Compiler
### Auxiliary tools
#### Ninja generator
#### Icecream tool
#### ccache tool
### LIBDEPS
#### Design
#### Linting and linter tags

## Build system configuration
### SCons configuration
#### Frequently used flags and variables
### MongoDB build configuration
#### Frequently used flags and variables
### Targets and Aliases

## Build artifacts and installation
### Hygienic builds
### AutoInstall
### AutoArchive

## MongoDB SCons style guide
### Sconscript Formatting Guidelines
#### Vertical list style
#### Alphabetize everything
### `Environment` Isolation
### Declaring Targets (`Program`, `Library`, and `CppUnitTest`)
### Invoking external tools correctly with `Command`s
### Customizing an `Environment` for a target
### Invoking subordinate `SConscript`s
#### `Import`s and `Export`s
### A Model `SConscript` with Comments
