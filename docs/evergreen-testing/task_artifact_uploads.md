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
prefix and object names, so restarted tasks can replace their own artifacts without changing
download paths. Rebuilt files may have different bytes. The uploader first reads the destination's
metadata with `HeadObject`:

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

Evergreen functions cannot invoke other functions. The producer migration introduces shared
`upload_task_artifact_exec` and `attach_task_artifact` YAML anchors to embed uploads in existing
functions. Callers override artifact parameters while preserving the executing task identity.
Function vars and `expansions.update` can nevertheless override those identity expansions. Path
validation and object metadata checks do not authenticate them; a task that supplies another task's
identity can bypass this convention while holding the shared write credentials.

## Adoption and enforcement

The uploader is available for use by Evergreen tasks. Existing producers and consumers retain their
current upload and download paths until they are migrated together in the next change. YAML and
generated-task lint enforcement is added after that migration.

The uploader establishes a configuration convention, not an AWS authorization boundary. Completing
that boundary requires Evergreen infrastructure to issue short-lived write credentials scoped to the
executing task's prefix and to remove broader shared write credentials from tasks. A trusted agent
or credential service must determine the task identity; a task cannot supply its own scope. The
scoped credentials should allow `s3:PutObject`, `s3:PutObjectAcl`, `s3:AbortMultipartUpload`, and
the `s3:GetObject` permission needed for `HeadObject` only beneath the task prefix, without
permission to delete objects. `s3:PutObjectAcl` is required because both `PutObject` and
`CreateMultipartUpload` set an explicit ACL, including for private uploads. `HeadObject` can return
403 for a missing object without bucket-list permission. In that case the uploader attempts only
`If-None-Match: *` creation, which cannot overwrite an existing object. Replacement requires a
successful ownership and ETag check; an unreadable existing object cannot be replaced. Credentials
for downloading other tasks' artifacts remain separate.

A bucket policy can additionally require conditional writes on the `task-artifacts` namespace. It
must allow both `If-None-Match` creation and `If-Match` replacement, and permit multipart initiation
and part uploads while requiring the condition on completion. Credential issuance and bucket policy
changes live outside this repository and are prerequisites for claiming that arbitrary task code
cannot bypass these checks.

Before enabling this on release-producing projects, validate signed artifact attachments, Windows
uploads, large multipart artifacts, dependent downloads, and build promotion in an Evergreen patch.
