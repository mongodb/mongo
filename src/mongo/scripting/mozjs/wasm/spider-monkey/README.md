# Updating SpiderMonkey

## Steps

1. **Update the version and checksum files:**

   - Set `spider-monkey-version` to the new Firefox/SpiderMonkey release tag
     (e.g. `FIREFOX_141_0esr_RELEASE`).
   - Update `spider-monkey-sha256sum` with the SHA-256 of the corresponding
     GitHub source archive. You can compute it with:
     ```
     curl -sL "https://github.com/mozilla-firefox/firefox/archive/refs/tags/<TAG>.tar.gz" | sha256sum
     ```

2. **Run the CI variant in a patch build:**

   - Create an Evergreen patch that includes your changes.
   - Run the `spidermonkey-wasm-compile` variant. This builds the
     `spidermonkey_wasip2_dist_release_from_source` target and uploads the
     resulting tarball to S3 under
     `spidermonkey-wasm/<spidermonkey_version>/`.

3. **Get the new tarball URL:**

   - Go to the patch build's **Files** tab in the Evergreen UI.
   - Copy the S3 URL for `spidermonkey-wasip2-release.tar.gz`.

4. **Update the spidermonkey_wasi dependency:**

   - Update the `spidermonkey_wasi` module's URL and SHA-256 to point at the
     newly uploaded tarball.

5. **Open a PR** linking the patch build that produced the tarball.
