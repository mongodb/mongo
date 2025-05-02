# S3 Binary

This is a small utility to help safely manage tool binaries that are stored in MongoDB's S3 bucket for the purpose of using in this repository's build, test, or release processes.

### Security

Any time a binary is pulled down from the internet and executed, there is risk that the binary has been modified unintentionally. This tool creates a hash of the binary that the developer is uploads and stores a record of it in a programmatically accessible Python script (see `buildscripts/s3_binary/hashes.py`). When a tool uses the S3 binary, this interface forces a checksum of the binary before the binary is run, verifying the result against the value stored in `hashes.py` and stopping execution if it doesn't match.

### Hermetic Guarantee

The other risk of relying on a binary stored in S3 is that if the binary is changed, that it will change the results of previously run tests or builds in continuous integration. This is not ideal since there are often cases where an old commit needs to be re-ran to reproduce user issues. Storing the hash in the repository and preventing modifications prevents accidental compatibility breaks of previous commits.

### Example Usage

Scenario: You have a developer tool called db-contrib-tool that you want to build into a binary, and then use that binary as part of a test process in 10gen/mongo. To use the s3_binary tool you would:

1. Create your binaries and put them into a single directory on your local system, ex:
   /tmp/db-contrib-tool/db-contrib-tool-v1_windows.exe
   /tmp/db-contrib-tool/db-contrib-tool-v1_linux

2. Invoke bazel run buildscripts/s3_binary:upload -- /tmp/db-contrib-tool s3://mdb-build-public/db-contrib-tool/v1

3. Follow the prompts, this will then update your local `buildscripts/s3_binary/hashes.py` file mapping the s3 path of each binary to its sha256 hash.

4. Update your test code to call: `download_s3_binary(f"s3://mdb-build-public/db-contrib-tool/v1/db-contrib-tool-v1_{os}{ext}")`. This will then automatically verify the download matches the hash at runtime.

5. Create a commit with your new code that adds in the `download_s3_binary` call and the `buildscripts/s3_binary/hashes.py` modifications.

The case above covers usage in Python. If using another language like starlark for Bazel dependencies, you would follow the same flow but copy the hashes into the starlark code instead of relying off of hashes.py. Please retain the modifications to hashes.py regardless to make it easy to use your binaries in python.

### Future Additions

In general, it's less error prone to have the entire flow of building, uploading, and using a binary all happen in an automated pipeline without developer interaction. In the future, this tool will be updated to be easily invocable from a continuous integration pipeline that performs the build and either returns the hashes to the user to be later committed, or automatically submits a PR to update them.
