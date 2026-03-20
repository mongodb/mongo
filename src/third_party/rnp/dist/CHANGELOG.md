## Changelog

### 0.18.1 [2025-11-21]

#### Security

* Fixed critical issue where PKESK (public-key encrypted) session keys were
  generated as all-zero, allowing trivial decryption of messages encrypted with
  public keys only (CVE-PENDING,
  https://bugzilla.redhat.com/show_bug.cgi?id=2415863).


### 0.18.0 [2025-05-24] -- RETRACTED, DON'T USE

**SECURITY WARNING:** This version contains a critical vulnerability where
public-key encrypted messages use all-zero session keys. All users must upgrade
to 0.18.1 or later immediately.

#### General

* Discourage use of EAX AEAD mode
* Generate RSA 3072-bit keys by default
* Support dearmoring of GnuPG-armored files (with ARMORED FILE header)
* Generate rnp_ver.h header
* Support DSA 4096 bit keys as some entities use them
* Mark signatures, produced by encrypt-only key or subkey, as invalid
* Allow extra spaces during armored key import
* Better support of Botan 3.5.0+
* Compatibility fixes for different systems
* Update hash function from the weak one on change of the key expiration
* Do not allow 64-bit ciphers for encryption without explicit option
* Internal refactoring and performance updates

#### FFI

* Added functions rnp_signature_error_count()/rnp_signature_error_at() to check why signature validation failed.
* Added functions to create and customize key certifications: rnp_key_certification_create(), rnp_key_signature_set_*/rnp_key_signature_get_*

### 0.17.1 [2024-04-08]

#### General

* Added support for Botan 3.
* Updated support for OpenSSL 3.
* Added support for mimemode in literal data packet.
* Relaxed Base64 decoding to allow spaces after the checksum.

#### FFI

* Added function `rnp_signature_get_features()`.

### 0.17.0 [2023-05-01]

#### General

* Added support for hidden recipient during decryption.
* Added support for AEAD-OCB for OpenSSL backend.
* Improve support for offline secret keys during default key selection.
* Support for GnuPG 2.3+ secret key store format.
* SExp parsing code is moved to separate library, https://github.com/rnpgp/sexp.
* Mark subkeys as expired instead of invalid if primary key is expired.
* AEAD: use OCB by default instead of EAX.
* Do not attempt to validate signatures of unexpected types.
* Use thread-safe time and date handling functions.
* Added ENABLE_BLOWFISH, ENABLE_CAST5 and ENABLE_RIPEMD160 build time options.
* Do not use `EVP_PKEY_CTX_set_dsa_paramgen_q_bits()` if OpenSSL backend version is < 1.1.1e.
* Corrected usage of CEK/KEK algorithms if those differs.

#### FFI

* Added function `rnp_signature_export()`.
* Added flag `RNP_VERIFY_ALLOW_HIDDEN_RECIPIENT` to `rnp_op_verify_set_flags()`.

#### CLI

* Added default armor message type for `--enarmor` command.
* Added command `--set-filename` to specify which file name should be stored in message.
* Added `--add-subkey` subcommand to the `--edit-key`.
* Added `set-expire` subcommand to the `--edit-key`.
* Added `--s2k-iterations` and `--s2k-msec` options to the `rnp`.
* Added `--allow-weak-hash` command to allow usage of weak hash algorithms.
* Report number of new/updated keys during the key import.

### 0.16.3 [2023-04-11]

#### Security

* Fixed issue with possible hang on malformed inputs (CVE-2023-29479).
* Fixed issue where in some cases, secret keys remain unlocked after use (CVE-2023-29480).

### 0.16.2 [2022-09-20]

#### General

* Fixed CMake issues with ENABLE_IDEA and ENABLE_BRAINPOOL.

### 0.16.1 [2022-09-06]

#### General

* Ensure support for RHEL9/CentOS Stream 9/Fedora 36, updating OpenSSL backend support for v3.0.
* Optional import and export of base64-encoded keys.
* Optional raw encryption of the data.
* Optional overriding of the current timestamp.
* Do not fail completely on unknown signature versions.
* Do not fail completely on unknown PKESK/SKESK packet versions.
* Support armored messages without empty line after the headers.
* Added automatic feature detection based on backend.

#### Security

* Separate security rules for the data and key signatures, extending SHA1 key signature support till the Jan, 19 2024.
* Set default key expiration time to 2 years.
* Limit maximum AEAD chunk bits to 16.

#### FFI

* Changed behaviour of `rnp_op_verify_execute()`: now it requires single valid signature to succeed.
* Added function `rnp_op_verify_set_flags()` to override default behaviour of verification.
* Added function `rnp_key_is_expired()`.
* Added function `rnp_op_encrypt_set_flags()` and flag `RNP_ENCRYPT_NOWRAP` to allow raw encryption.
* Added flag `RNP_LOAD_SAVE_BASE64` to the function `rnp_import_keys()`.
* Added flag `RNP_KEY_EXPORT_BASE64` to the function `rnp_key_export_autocrypt()`.
* Added function `rnp_set_timestamp()` to allow to override current time.
* Update security rules functions with flags `RNP_SECURITY_VERIFY_KEY` and `RNP_SECURITY_VERIFY_DATA`.

#### CLI

* Make password request more verbose.
* Print `RSA` instead of `RSA (Encrypt and Sign)` in the key listing to avoid confusion.
* Added option `--source` to specify detached signature's source file.
* Added option `--no-wrap` to allow raw data encryption.
* Added option `--current-time` to allow to override current timestamp.
* Strip known extensions (like `.pgp`, `.asc`, etc.) when decrypting or verifying data.
* Display key and signature validity status in the key listing.
* Do not attempt to use GnuPG's config to set default key.

### 0.16.0 [2022-01-20]

#### General

* Added support for OpenSSL cryptography backend so RNP may be built and used on systems without the Botan installed.
* Added compile-time switches to disable certain features (AEAD, Brainpool curves, SM2/SM3/SM4 algorithms, Twofish)
* Fixed possible incompatibility with GnuPG on x25519 secret key export from RNP to GnuPG.
* Fixed building if Git is not available.
* Fixed export of non-FFI symbols from the rnp.so/rnp.dylib.
* Fixed support for Gnu/Hurd (absence of PATH_MAX).
* Added support for `None` compression algorithm.
* Added support for the dumping of notation data signature subpackets.
* Fixed key expiration time calculation in the case with newer non-primary self-certification.
* Improved performance of key import (no key material checks)

#### Security

* Added initial support for customizable security profiles.
* Mark SHA1 signatures produced later than 2019-01-19, as invalid.
* Mark MD5 signatures produced later than 2012-01-01, as invalid.
* Remove SHA1 and 3DES from the default key preferences.
* Use SHA1 collision detection code when using SHA1.
* Mark signatures with unknown critical notation as invalid.
* Do not prematurely mark secret keys as valid.
* Validate secret key material before the first operation.
* Limit the number of possible message recipients/signatures to a reasonable value (16k).
* Limit the number of signature subpackets during parsing.

#### FFI

* Added functions `rnp_backend_string()` and `rnp_backend_version()`.
* Added functions `rnp_key_25519_bits_tweaked()` and `rnp_key_25519_bits_tweak()` to check and fix x25519 secret key bits.
* Added security profile manipulation functions: `rnp_add_security_rule()`, `rnp_get_security_rule()`, `rnp_remove_security_rule()`.
* Added function `rnp_signature_get_expiration()`.
* Deprecate functions `rnp_enable_debug()`/`rnp_disable_debug()`.

#### CLI

* Write new detailed help messages for `rnp` and `rnpkeys`.
* Added `-` (stdin) and `env:VAR_NAME` input specifiers, as well as `-` (stdout) output specifier.
* Do not fail with empty keyrings if those are not needed for the operation.
* Added algorithm aliases for better usability (i.e. `SHA-256`, `SHA256`, etc.).
* Added option `--notty` to print everything to stdout instead of TTY.
* Added command `--edit-key` with subcommands `--check-cv25519-bits` and `--fix-cv25519-bits`.
* Remove support for `-o someoption=somevalue`, which is unused.
* Remove no longer used support for additional debug dumping via `--debug source.c`.

### 0.15.2 [2021-07-20]

#### General

* Be less strict in userid validation: allow to use userids with self-signature, which has key expiration in the past.
* Do not mark signature as invalid if key which produced it is expired now, but was valid during signing.
* Fix incorrect key expiration calculation in some cases.
* Fix incorrect version number in the `version.txt`.

#### FFI

* Add function `rnp_key_get_default_key()` to pick the default key/subkey for the specific operation.
* Allow to pass NULL hash parameter to `rnp_key_add_uid()` to pick the default one.
* Use the same approach as in `rnp_op_encrypt_add_recipient()` for encryption subkey selection in `rnp_key_export_autocrypt()`.

#### CLI

* `rnp`: Show error message if encryption failed.
* `rnpkeys` : Add `--expiration` option to specify expiration time during key generation.

### 0.15.1 [2021-05-28]

#### General

* Make man pages building optional.
* Fixed updating of expiration time for a key with multiple user ids.
* Fixed key expiry check for keys valid after the year 2038.
* Pick up key expiration time from direct-key signature or primary userid certification if available.

#### FFI

* Added function `rnp_key_valid_till64()` to correctly handle keys which expire after the year 2038.
* Added `RNP_FEATURE_*` defines to be used instead of raw strings.

#### Security

* Fixed issue with cleartext key data after the `rnp_key_unprotect()`/`rnp_key_protect()` calls (CVE-2021-33589).

### 0.15.0 [2021-04-04]

#### General

* Added CMake options to allow offline builds, i.e. without Googletest/ruby-rnp downloads.
* Removed major library version from the library name (librnp-0.so/dll -> librnp.so/dll).
* Improved handling of cleartext signatures, when empty line between headers and contents contains some whitespace characters.
* Relaxed requirements for the armored messages CRC (allow absence of the CRC, and issue warning instead of complete failure).
* Updated build instructions for MSVC.
* Improved support of 32-bit platforms (year 2038 problem).

#### CLI

* Added up-to-date manual pages for `rnp` and `rnpkeys`.
* rnpkeys: added `--remove-key` command.

#### FFI

* Added up-to-date manual page for `librnp`.
* Added function `rnp_signature_remove`
* Added function `rnp_uid_remove`
* Added function `rnp_key_remove_signatures` for batch signature removal and filtering.

### 0.14.0 [2021-01-15]

#### General

* Improved key validation: require to have at least one valid, non-expiring self signature.
* Added support for 'stripped' keys without userids and certifications but with valid subkey binding signature.
* Added support for Windows via MinGW/MSYS2.
* Added support for Windows via MSVC.
* Fixed secret key locking when it is updated with new signatures/subkeys.
* Fixed key expiry/flags calculation (take in account only the latest valid self-signature/subkey binding).
* Fixed MDC reading if it appears on 8k boundary.
* Disabled logging by default in release builds and added support for environment variable `RNP_LOG_CONSOLE` to enable it back.
* Fixed leading zeroes for secp521r1 b & n field constants.
* Allowed keys and signatures with invalid MPI bit count.
* Added support for private/experimental signature subpackets, used by GnuPG and other implementations.
* Added support for reserved/placeholder signatures.
* Added support for zero-size userid/attr packet.
* Relaxed packet dumping, ignoring invalid packets and allowing to find wrong packet easier.
* Improved logging of errored keys/subkeys information for easier debugging.
* Fixed support for old RSA sign-only/encrypt-only and ElGamal encrypt-and-sign keys.
* Fixed support for ElGamal keys larger then 3072 bits.
* Fixed symbol visibility so only FFI functions are exposed outside of the library.
* Added support for unwrapping of raw literal packets.
* Fixed crash with non-detached signature input, fed into the `rnp_op_verify_detached_create()`.
* Significantly reduced memory usage for the keys large number of signatures.
* Fixed long armor header lines processing.
* Added basic support for GnuPG's offline primary keys (`gnupg --export-secret-subkeys`) and secret keys, stored on card.
* Fixed primary key binding signature validation when hash algorithm differs from the one used in the subkey binding signature.
* Fixed multiple memory leaks related to invalid algorithms/versions/etc.
* Fixed possible crashes during processing of malformed armored input.
* Limited allowed nesting levels for OpenPGP packets.
* Fixed support for text-mode signatures.
* Replaced strcpy calls with std::string and memcpy where applicable.
* Removed usage of mktemp, replacing it with mkstemp.
* Replaced usage of deprecated `botan_pbkdf()` with `botan_pwdhash()`.
* Added support for the marker packet, issued by some implementations.
* Added support for unknown experimental s2ks.
* Fixed armored message contents detection (so armored revocation signature is not more reported as the public key).
* Changed behaviour to use latest encryption subkey by default.
* Fixed support for widechar parameters/file names on Windows.
* Implemented userid validity checks so only certified/non-expired/non-revoked userid may be searched.
* Fixed GnuPG compatibility issues with CR (`\r`) characters in text-mode and cleartext-signed documents.
* Improved performance of the key/uid signatures access.
* Migrated tests to the Python 3.
* Migrated most of the internal code to C++.

#### CLI

* Do not load keyring when it is not required, avoiding extra `keyring not found` output.
* Input/output data via the tty, if available, instead of stdin/stdout.
* Fixed possible crash when HOME variable is not set.
* rnpkeys: Added `--import-sigs` and changed behavior of `--import` to check whether input is key or signature.
* rnpkeys: Added `--export-rev` command to export key's revocation, parameters `--rev-type`, `--rev-reason`.
* rnpkeys: Added `--revoke-key` command.
* rnpkeys: Added `--permissive` parameter to `--import-keys` command.
* rnpkeys: Added `--password` options, allowing to specify password and/or generate unprotected key.

#### FFI

* Added keystore type constants `RNP_KEYSTORE_*`.
* Added `rnp_import_signatures`.
* Added `rnp_key_export_revocation`.
* Added `rnp_key_revoke`.
* Added `rnp_request_password`.
* Added `rnp_key_set_expiration` to update key's/subkey's expiration time.
* Added flag `RNP_LOAD_SAVE_PERMISSIVE` to `rnp_import_keys`, allowing to skip erroneous packets.
* Added flag `RNP_LOAD_SAVE_SINGLE`, allowing to import keys one-by-one.
* Added `rnp_op_verify_get_protection_info` to check mode and cipher used to encrypt message.
* Added functions to retrieve recipients information (`rnp_op_verify_get_recipient_count`, `rnp_op_verify_get_symenc_count`, etc.).
* Added flag `RNP_KEY_REMOVE_SUBKEYS` to `rnp_key_remove` function.
* Added function `rnp_output_pipe` allowing to write data from input to the output.
* Added function `rnp_output_armor_set_line_length` allowing to change base64 encoding line length.
* Added function `rnp_key_export_autocrypt` to export public key in autocrypt-compatible format.
* Added functions to retrieve information about the secret key's protection (`rnp_key_get_protection_type`, etc.).
* Added functions `rnp_uid_get_type`, `rnp_uid_get_data`, `rnp_uid_is_primary`.
* Added function `rnp_uid_is_valid`.
* Added functions `rnp_key_get_revocation_signature` and `rnp_uid_get_revocation_signature`.
* Added function `rnp_signature_get_type`.
* Added function `rnp_signature_is_valid`.
* Added functions `rnp_key_is_valid` and `rnp_key_valid_till`.
* Added exception guard to FFI boundary.
* Fixed documentation for the `rnp_unload_keys` function.

#### Security

* Removed version header from armored messages (see https://mailarchive.ietf.org/arch/msg/openpgp/KikdJaxvdulxIRX_yxU2_i3lQ7A/ ).
* Enabled fuzzing via oss-fuzz and fixed reported issues.
* Fixed a bunch of issues reported by static analyzer.
* Require at least Botan 2.14.0.

### 0.13.1 [2020-01-15]
#### Security

* rnpkeys: Fix issue #1030 where rnpkeys would generate unprotected secret keys.

### 0.13.0 [2019-12-31]
#### General

* Fixed a double-free on invalid armor headers.
* Fixed broken versioning when used as a git submodule.
* Fixed an infinite loop on parsing truncated armored keys.
* Fixed armored stream parsing to be more flexible and allow blank lines before trailer.
* Fixed the armor header for detached signatures (previously MESSAGE, now SIGNATURE).
* Improved setting of default qbits for DSA.
* Fixed a crash when retrieving signature revocation reason.
* Stop using expensive tests for key material validation.

#### CLI

* rnpkeys: Removed a few redundant commands (--get-key, --print-sigs, --trusted-keys, ...).
* rnpkeys: Added --secret option.
* rnpkeys: Display 'ssb' for secret subkeys.
* rnp: Added `--list-packets` parameters (`--json`, etc.).
* rnp: Removed `--show-keys`.

#### FFI

* Added `rnp_version_commit_timestamp` to retrieve the commit timestamp
  (for non-release builds).
* Added a new (non-JSON) key generation API (`rnp_op_generate_create` etc.).
* Added `rnp_unload_keys` function to unload all keys.
* Added `rnp_key_remove` to unload a single key.
* Expanded bit length support for JSON key generation.
* Added `rnp_key_get_subkey_count`/`rnp_key_get_subkey_at`.
* Added various key property accessors (`rnp_key_get_bits`, `rnp_key_get_curve`).
* Added `rnp_op_generate_set_protection_password`.
* Added `rnp_key_packets_to_json`/`rnp_dump_packets_to_json`.
* Added `rnp_key_get_creation`, `rnp_key_get_expiration`.
* Added `rnp_key_get_uid_handle_at`, `rnp_uid_is_revoked`, etc.
* Added `rnp_key_is_revoked` and related functions to check for revocation.
* Added `rnp_output_to_path` and `rnp_output_finish`.
* Added `rnp_import_keys`.
* Added `rnp_calculate_iterations`.
* Added `rnp_supports_feature`/`rnp_supported_features`.
* Added `rnp_enable_debug`/`rnp_disable_debug`.
* Added `rnp_key_get_primary_grip`.
* Added `rnp_output_to_armor`.
* Added `rnp_op_generate_set_request_password`.
* Added `rnp_dump_packets_to_output`.
* Added `rnp_output_write`.
* Added `rnp_guess_contents`.
* Implemented `rnp_op_set_file_name`/`rnp_op_set_file_mtime`.
* Added `rnp_op_encrypt_set_aead_bits`.
* Added `rnp_op_verify_signature_get_handle`.
* Added `rnp_signature_packet_to_json`.

#### Packaging

* RPM: Split packages into librnp0, librnp0-devel, and rnp0.

### 0.12.0 [2019-01-13]
#### General

* We now require Botan 2.8+.
* Fixed key grip calculations for various key types.
* Fixed SM2 signatures hashing the hash of the message. See comment in issue #436.
* Added support for G10 ECC keys.
* Fixed dumping of partial-length packets.
* Added support for extra ECC curves:
  * Brainpool p256, p384, p512 ECDSA/ECDH
  * secp256k1 ECDSA/ECDH
  * x25519
* Fixed AEAD with newer versions of Botan.
* Removed a lot of legacy code.

#### CLI

* rnp: Added -f/--keyfile option to load keys directly from a file.
* rnp: Fixed issue with selecting G10 secret keys via userid.
* rnpkeys: Added support for SM2 with arbitrary hashes.
* redumper: Added -g option to dump fingerprints and grips.
* redumper: Display key id/fingerprint/grip in packet listings.

#### FFI

* Added FFI examples.
* Fixed a regression with loading subkeys directly.
* Implemented support for per-signature hash and creation/expiration time.
* Added AEAD support.

### 0.11.0 [2018-09-16]
#### General

* Remove some old SSH key support.
* Add support for dynamically calculating the S2K iterations.
* Add support for extracting the public key from the secret key.
* Add support for merging information between keys.

#### CLI

* Add options for custom S2K iterations/times (dynamic by default).

### 0.10.0 [2018-08-20]
#### General

* Fixed some compiler warnings.
* Switched armoring to use PRIVATE KEY instead of SECRET KEY.

#### ECDSA

* Use the matching hash to be used for the deterministic nonce generation.
* Check that the input is of the expected length.
* Removed the code to truncate the ECDSA input since this is now handled by Botan.

#### FFI

* Added enarmor and dearmor support.
* Added library version retrieval.
* Removed rnp_export_public_key, added rnp_key_export.


### 0.9.2 [2018-08-13]
#### General

* Support for generation and verification of embedded signature subpacket for signing subkeys
* Verification of public key signatures and key material
* Improved performance of asymmetric operations (key material is now validated on load)

#### FFI

* Fixed `rnp_op_add_signature` for G10 keys


### 0.9.1 [2018-07-12]
#### General

* Added issuer fingerprint to certifications and subkey bindings.

#### CLI

* Added support for keyid/fpr usage with (some) spaces and 0x prefix in
  operations (`--sign`, etc).

#### FFI

* Fixed key search by fingerprint.


### 0.9.0 [2018-06-27]
* First official release.
