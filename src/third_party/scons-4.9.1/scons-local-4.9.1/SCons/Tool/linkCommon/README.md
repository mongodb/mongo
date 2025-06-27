# Current implementation details

The code has been simplified to use Subst expansion do most of the work.

The following internal env vars have been added which are python functions (note also ldmodule where shlib is below)
* `_get_shlib_stem` - given a library name `libxyz.so.1.1.0` it will retrievve `xyz`
* `_get_shlib_dir` - given a library with a path `DEF/a/b/libxyz.so.1.1.0` it will retrieve `DEF`
* `_SHLIBSOVERSION` - will look for `SOVERSION` and then `SHLIBVERSION` to determine the `SOVERSION` to be used in file naming
* `_SHLIBSONAME` - will check for and error if both `SONAME` and `SOVERSION` are defined, otherwise will generate the proper filenaming for SONAME
* `_SHLIBVERSION` - will return `SHLIBVERSION` or "" if SHLIBVERSION is defined
* `_SHLIBVERSIONFLAGS` - will return proper compiler flags for SHLIBVERSION
* `__SHLIBVERSIONFLAGS` - compiler defined proper versioned shared library flags
* `SHLIB_SONAME_SYMLINK` - this is the symlink for SONAME to be used (`libxyz.1.so`)
* `SHLIB_NOVERSION_SYMLINK` - this is the symlink for non-versioned filename (`libxyz.so`)
Shared library file nodes will have `node.attributes.shliblinks` which is a tuple of the symlinks to be created



# Versioned Shared Library and Loadable modules requirements

The following env variables can affect the command line and created files for these

* `SHLIBVERSION` - If this is not set, the all of the following will be ignored?
* `SONAME`
* `SOVERSION`
* `APPLELINK_NO_CURRENT_VERSION`    (applelink only)
* `APPLELINK_CURRENT_VERSION`       (applelink only)
* `APPLELINK_COMPATIBILITY_VERSION` (applelink only)

In most cases the linker will create a file named as

`${SHLIBPREFIX}lib_name${SHLIBVERSION}${SHLIBSUFFIX}`

Which will have a soname baked into it as one of the

* `${SONAME}`
* `${SHLIBPREFIX}lib_name${SOVERSION}${SHLIBSUFFIX}`
* `-Wl,-soname=$_SHLIBSONAME` (for gnulink as similar)
* (for applelink only)
   * `${SHLIBPREFIX}lib_name${major version only from SHLIBVERSION}${SHLIBSUFFIX}`
   * `-Wl,-compatibility_version,%s`
   * `-Wl,-current_version,%s`
   
For **applelink** the version has to follow these rules to verify that the version # is valid.

* For version # = X[.Y[.Z]]
* where X 0-65535
* where Y either not specified or 0-255
* where Z either not specified or 0-255

   
For most platforms this will lead to a series of symlinks eventually pointing to the actual shared library (or loadable module file).
1. `${SHLIBPREFIX}lib_name${SHLIBSUFFIX} -> ${SHLIBPREFIX}lib_name${SHLIBVERSION}${SHLIBSUFFIX}`
1. `${SHLIBPREFIX}lib_name${SOVERSION}${SHLIBSUFFIX} -> ${SHLIBPREFIX}lib_name${SHLIBVERSION}${SHLIBSUFFIX}`

These symlinks are stored by the emitter in the following
`target[0].attributes.shliblinks = symlinks`
This means that those values are fixed a the time SharedLibrary() is called (generally)

For **openbsd** the following rules for symlinks apply

   * OpenBSD uses x.y shared library versioning numbering convention and doesn't use symlinks to backwards-compatible libraries



User can request:
env.SharedLibrary('a',sources, SHLIBVERSION)
env.SharedLibrary('liba.so',sources, SHLIBVERSION)
Ideally we'll keep the 'a' for use in constructing all follow on. To do this we have to do it in the Builder() or at
least prevent  BuilderBase._create_nodes() from discarding this info if it's available.


Firstly check if [SH|LD]LIBNOVERSIONSYMLINKS defined or if [SH|LD]LIBVERSION is not defined, if so we do nothing special

The emitter can calculate the filename stem 'a' above and store it on the target node. Then also create the symlinks
and store those on the node. We should have all the information needed by the time the emitter is called.
Same should apply for loadable modules..
This should be vastly simpler.
Unfortunately we cannot depend on the target having an OverrideEnvironment() which we could populate all the related
env variables in the emitter...
Maybe we can force one at that point?


SOVERSION can be specified, if not, then defaults to major portion of SHLIBVERSION
SONAME can be specified, if not defaults to ${SHLIBPREFIX}lib_name${SOVERSION}

NOTE: mongodb uses Append(SHLIBEMITTER=.. )  for their libdeps stuff. (So test 
with that once you have new logic working)

