# Task-owned internal build artifacts

Tasks can opt into the `upload task artifact` Evergreen function. Its uploader constructs the
destination from the supplied task identity expansions:

```text
s3://mciuploads/${project}/${build_variant}/${version_id}/task-artifacts/${task_name}/${relative_name}
```

`compile_variant` selects the producer for downloads. It does not select an upload destination. Two
variants that reuse the same compile have separate upload prefixes when their identity expansions
are preserved, and two different tasks on one variant have separate prefixes.

## Uploading

```yaml
- func: "upload task artifact"
  vars:
    artifact_local_file: src/bazel-bin/dist-test-stripped.tgz
    artifact_relative_name: binaries/mongo.tgz
    artifact_display_name: Binaries
    artifact_content_type: application/gzip
```

The task must already have a source checkout and the task Python venv. Relative local paths are
resolved against the Evergreen workdir, including Cygwin paths on Windows. Relative object names
allow letters, numbers, underscores, dots, hyphens, and slash-separated directories. Empty segments,
absolute paths, and `.` or `..` segments are rejected after expansion.

The uploader defaults to private objects with signed Evergreen artifact links. It retains existing
display names so artifact-based consumers such as sys-perf and db-contrib-tool can find the files.
The attachment JSON contains project credential variable names, never AWS secrets. Public uploads
must select both `artifact_permissions: public-read` and `artifact_visibility: public` where an
existing internal consumer requires them. Mismatched permissions and visibility are rejected before
any S3 request. Both upload paths use S3 Intelligent-Tiering, matching Evergreen's `s3.put` command.

`artifact_content_type` accepts only `application/octet-stream`, `application/gzip`,
`application/x-gzip`, `application/tar`, `application/x-tar`, `application/zip`, `application/json`,
`application/yaml`, `text/plain`, and `text/yaml`. Active web types such as HTML, JavaScript, and
SVG are rejected before any S3 request, including for optional files. Both single-part and multipart
uploads set `Content-Disposition: attachment`, so arbitrary file bytes are downloaded instead of
rendered from the shared bucket origin.

`artifact_optional: "true"` skips only a missing local file. All executions of a task share one
prefix and object names. Ordinary patches replace their own artifacts without requiring release
inputs, unless explicitly opted in with `is_release: true` or `artifact_release_build: true`. Tagged
builds with `is_release: true` and non-patch builds in `mongo-release*` projects preserve existing
artifacts. Publishing variants outside `mongodb-mongo-master` also preserve non-patch uploads when
`push_bucket` is set, covering included module variants and inherited YAML expansions. Producers
that lack these signals can opt in with `artifact_release_build: true`; ordinary release variants
need no additional marker. Bazel release arguments control compilation separately. An upload creates
an absent object with `If-None-Match: *`. If an object already exists and belongs to this task, the
uploader succeeds, logs that rebuilt bytes were not uploaded, and attaches the existing object's
size and content type. This policy covers binaries, provenance JSON, and execution logs together.
Replacing a release artifact requires manual S3 deletion; restarting alone does not replace it.
Conditional creation races succeed only after verifying the existing object's ownership. Preserved
objects have the requested ACL reapplied before attachment; a failed ACL update fails the upload and
leaves the attachment empty. A wrong producer, newer recorded execution, or unreadable existing
object still fails rather than being silently accepted. Bucket policy must enforce conditional
creation to prevent other upload code from replacing release objects.

Development tasks can replace their own artifacts without changing download paths. Rebuilt files may
have different bytes. The uploader first reads the destination's metadata with `HeadObject`:

- A missing object is created using `If-None-Match: *`.
- An existing object must have matching `task-id`, `task-name`, and `build-variant` metadata and a
  recorded execution no newer than the current one. The replacement uses `If-Match` with the
  observed ETag, and records the current execution in the new object's metadata.
- An object without valid ownership metadata, an observed newer execution, or a denied request fails
  the task. Changes to the ETag between the check and the write also fail the task.

The condition is applied to `PutObject`, or to `CompleteMultipartUpload` for large files. Incomplete
multipart uploads are aborted on failure. A failed upload clears the artifact manifest; a successful
replacement attaches the same download URL with the new file size.

ETags guard against concurrent changes to object content, not metadata-only changes. This is not a
lock across task executions or an atomic publication of all files: avoid running producer executions
concurrently, and restart dependent tasks if they need the rebuilt artifacts.

## Package provenance

`test_packages` resolves dependencies by Evergreen task IDs starting from its executing task, then
selects package, archive, and crypt producers from that chain. Producer tasks must belong to the
test's build, variant, and version. Shared version generation is an infrastructure dependency and
does not supply binary artifacts.

The producer creates and uploads its own provenance JSON. Package tests require its `task_id`,
`task_name`, `build_id`, `build_variant`, and `version_id` to match independently fetched Evergreen
metadata, in addition to checking the build command, distro, and binary hashes. Missing origin
fields fail validation. Earlier executions of the correct task remain valid after release restarts.
Distro validation uses the execution recorded in the JSON, fetched independently from Evergreen;
future or malformed execution values fail. Both the current and recorded executions must use release
distros. Archive producers build their own binaries on waterfall versions and explicit release
patches so their provenance retains the primary build command; ordinary development patches can
still reuse an earlier compile. Byte-for-byte reproducibility of rebuilt tarballs is not required.

The new archive-provenance JSON uses a task-specific filename under the existing artifacts
namespace. Existing binary destinations and native `s3.put` uploads are unchanged. Uploader adoption
and S3 path migration are tracked separately in SERVER-137930.

Producers can enable release recovery with `artifact_release_provenance_file`,
`artifact_release_execution_log_file`, and `artifact_release_related_files`. All input paths are
relative to the Evergreen workdir, just like `artifact_local_file`. Related files are a JSON array
of optional paths, which permits spaces and repeated basenames without losing inputs:

```yaml
artifact_release_provenance_file: src/release_binary_provenance.json
artifact_release_execution_log_file: src/release_execution_log.binpb.zst
artifact_release_related_files: >-
  ["src/bazel-bin/dist-debug.tgz", "src/extra data/provenance.json"]
```

Configured provenance and execution-log files must be regular files when creating a checkpoint, and
must be present in a restored checkpoint. Duplicate logical names and repeated destination paths are
rejected. The uploader checkpoints the archive and sidecars together before publication, and
restores them on retry before completing ordinary uploads. This adds one stored copy and a
checkpoint download. The checkpoint is a private object in `mciuploads`, at the archive's task-owned
key with `.release-inputs.tar` appended. Its upload must complete before publishing any individual
archive or sidecar. An interrupted checkpoint upload exposes no partial object or new individual
artifacts, so a retry can create a checkpoint from its new build. If the checkpoint completed but a
later upload failed (or its completion response was lost), the retry restores the original inputs.

Checkpoint creation uses `If-None-Match: *`, including when HEAD returns 403/AccessDenied for a
possibly absent key. If another writer already created it, the uploader must read and validate the
winning checkpoint. Archives record that checkpoint's ETag in `release-checkpoint-etag` metadata.
The uploader verifies this link before restoring over local inputs and before preserving an existing
archive, including one discovered after a conditional creation race. A missing or mismatched link
requires manual recovery, preventing a new checkpoint from relabelling an old archive. Unreadable
existing objects fail; an ambiguous HEAD can only permit conditional creation. This requires object
read and write permissions but no `s3:ListBucket`. A denied archive read may leave a private
checkpoint behind, but cannot publish mismatched binaries or sidecars.

Checkpoints are uncompressed tar files capped at 100 GiB. Downloads and member sizes are bounded and
checked against available disk space. Every member is validated and staged before any local input is
removed or replaced. Producer wiring is added with the migration.

Evergreen functions cannot invoke other functions. The producer migration introduces shared
`upload_task_artifact_exec` and `attach_task_artifact` YAML anchors to embed uploads in existing
functions. Callers override artifact parameters while preserving the executing task identity.
Function vars and `expansions.update` can nevertheless override those identity expansions. Path
validation and object metadata checks do not authenticate them; a task that supplies another task's
identity can bypass this convention while holding the shared write credentials.

## Adoption and enforcement

The uploader is available for use by Evergreen tasks. Existing producers and consumers retain their
current upload and download paths until they are migrated together after uploader infrastructure and
provenance validation are in place. YAML and generated-task lint enforcement is added after that
migration.

The uploader establishes a configuration convention, not an AWS authorization boundary. Completing
that boundary requires Evergreen infrastructure to issue short-lived write credentials scoped to the
executing task's prefix and to remove broader shared write credentials from tasks. A trusted agent
or credential service must determine the task identity; a task cannot supply its own scope. The
scoped credentials should allow `s3:PutObject`, `s3:PutObjectAcl`, `s3:AbortMultipartUpload`, and
the `s3:GetObject` permission needed for `HeadObject` only beneath the task prefix, without
permission to delete objects. `s3:PutObjectAcl` is required because both `PutObject` and
`CreateMultipartUpload` set an explicit ACL, including for private uploads. `HeadObject` can return
403 for a missing object without bucket-list permission. Both development and release uploads then
attempt only `If-None-Match: *` creation, which cannot overwrite an existing object. A refused
release creation requires a successful read and ownership check before preservation; recovery also
requires the archive to reference the winning checkpoint. Development replacement requires a
successful ownership and ETag check. An unreadable existing object cannot be preserved or replaced.
The uploader never lists the bucket, so these workflows do not require `s3:ListBucket`. Credentials
for downloading other tasks' artifacts remain separate.

A bucket policy can additionally require conditional writes on the `task-artifacts` namespace. It
must allow both `If-None-Match` creation and `If-Match` replacement, and permit multipart initiation
and part uploads while requiring the condition on completion. Credential issuance and bucket policy
changes live outside this repository and are prerequisites for claiming that arbitrary task code
cannot bypass these checks.

Writer-supplied metadata also cannot authenticate release binaries or checkpoints against a writer
who already has shared bucket credentials. Such a writer can plant matching metadata and bytes.
Task-scoped credentials from a trusted issuer, removal of shared write credentials, and conditional
bucket policy are required before release rollout; uploader-side checks cannot establish this
boundary. These changes prepare CI artifact handling and do not establish a server vulnerability or
a CVE disclosure requirement.

Before enabling this on release-producing projects, validate signed artifact attachments, Windows
uploads, large multipart artifacts, dependent downloads, and build promotion in an Evergreen patch.
