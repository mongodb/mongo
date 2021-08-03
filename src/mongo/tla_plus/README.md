Replication TLA+ Specifications
===============================

These are formal specifications for exploring possible replication protocols.
Some are experiments, some reflect MongoDB's actual implementation. See the
comments in each spec for details.

Some specs are intended for model-checking. They are in subdirectories like:

```
SpecName/
    SpecName.tla     specification
    MCSpecName.tla   additional operators for model-checking
    MCSpecName.cfg   configuration for model-checking
```

Run the model-checker on a spec thus:

```
./model-check.sh SpecName
```

There may be additional instructions in the spec or config file.
