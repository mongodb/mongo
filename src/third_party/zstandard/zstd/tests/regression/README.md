# Regression tests

The regression tests run zstd in many scenarios and ensures that the size of the compressed results doesn't change. This helps us ensure that we don't accidentally regress zstd's compression ratio.

These tests get run every night by CircleCI. If the job fails you can read the diff printed by the job to ensure the change isn't a regression. If all is well you can download the `results.csv` artifact and commit the new results. Or you can rebuild it yourself following the instructions below.

## Rebuilding results.csv

From the root of the zstd repo run:

```
# Build the zstd binary
make clean
make -j zstd

# Build the regression test binary
cd tests/regression
make clean
make -j test

# Run the regression test
./test --cache data-cache --zstd ../../zstd --output results.csv

# Check results.csv to ensure the new results are okay
git diff

# Then submit the PR
```
