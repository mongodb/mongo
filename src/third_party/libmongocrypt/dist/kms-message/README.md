# kms-message
Library used to generate requests for:
- Amazon Web Services Key Management Service (KMS)
- Azure Key Vault

This library is *not* a complete implementation of a KMS client, it only
implements the request format.

## Testing kms-message
- `test_kms_request` tests HTTP request generation and response parsing, but does not require internet or use any live servers.
- `test_kms_azure_online` makes live requests, and has additional requirements (must have working credentials).

### Requirements
- A complete installation of the C driver. (libbson is needed for parsing JSON, and libmongoc is used for creating TLS streams). See http://mongoc.org/libmongoc/current/installing.html for installation instructions. For macOS, `brew install mongo-c-driver` will suffice.
- An Azure key vault, and a service principal with an access policy allowing encrypt / decrypt key operations. The following environment variables must be set:
    - AZURE_TENANT_ID
    - AZURE_CLIENT_ID
    - AZURE_CLIENT_SECRET
    - AZURE_KEY_URL (e.g. `https://key-vault-kevinalbs.vault.azure.net/keys/test-key/9e1159e6ee5b447ba17e850b779bf652`)

### Building
Configure and build with cmake:
```
mkdir cmake-build
cd cmake-build
cmake ..
cmake --build . --target all
```

If the C driver is installed in a non-default location, specify the location with `-DCMAKE_PREFIX_PATH=...`.

To build tests with verbose (and insecure) tracing, define `TEST_TRACING_INSECURE` in compiler flags by specifying `-DCMAKE_C_FLAGS="-DTEST_TRACING_INSECURE"` on cmake configuration.

Recommended: compile tests with address sanitizer (use a relatively new gcc / clang compiler) by specifying `-fsanitize=address` in the C flags. This can be done by specifygin `-DCMAKE_C_FLAGS="-fsanitize=address"` as an option to cmake. Enable leak detection with the environment variable `ASAN_OPTIONS='detect_leaks=1'. Example:

```
cd cmake-build
cmake -DCMAKE_C_FLAGS="-fsanitize=address -DTEST_TRACING_INSECURE"
export ASAN_OPTIONS='detect_leaks=1'
./cmake-build/kms-message/test_kms_azure_online
```
