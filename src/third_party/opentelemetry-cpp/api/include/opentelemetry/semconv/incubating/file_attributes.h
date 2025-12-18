/*
 * Copyright The OpenTelemetry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * DO NOT EDIT, this is an Auto-generated file from:
 * buildscripts/semantic-convention/templates/registry/semantic_attributes-h.j2
 */

#pragma once

#include "opentelemetry/common/macros.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace semconv
{
namespace file
{

/**
  Time when the file was last accessed, in ISO 8601 format.
  <p>
  This attribute might not be supported by some file systems — NFS, FAT32, in embedded OS, etc.
 */
static constexpr const char *kFileAccessed = "file.accessed";

/**
  Array of file attributes.
  <p>
  Attributes names depend on the OS or file system. Here’s a non-exhaustive list of values expected
  for this attribute: @code archive @endcode, @code compressed @endcode, @code directory @endcode,
  @code encrypted @endcode, @code execute @endcode, @code hidden @endcode, @code immutable @endcode,
  @code journaled @endcode, @code read @endcode, @code readonly @endcode, @code symbolic link
  @endcode, @code system @endcode, @code temporary @endcode, @code write @endcode.
 */
static constexpr const char *kFileAttributes = "file.attributes";

/**
  Time when the file attributes or metadata was last changed, in ISO 8601 format.
  <p>
  @code file.changed @endcode captures the time when any of the file's properties or attributes
  (including the content) are changed, while @code file.modified @endcode captures the timestamp
  when the file content is modified.
 */
static constexpr const char *kFileChanged = "file.changed";

/**
  Time when the file was created, in ISO 8601 format.
  <p>
  This attribute might not be supported by some file systems — NFS, FAT32, in embedded OS, etc.
 */
static constexpr const char *kFileCreated = "file.created";

/**
  Directory where the file is located. It should include the drive letter, when appropriate.
 */
static constexpr const char *kFileDirectory = "file.directory";

/**
  File extension, excluding the leading dot.
  <p>
  When the file name has multiple extensions (example.tar.gz), only the last one should be captured
  ("gz", not "tar.gz").
 */
static constexpr const char *kFileExtension = "file.extension";

/**
  Name of the fork. A fork is additional data associated with a filesystem object.
  <p>
  On Linux, a resource fork is used to store additional data with a filesystem object. A file always
  has at least one fork for the data portion, and additional forks may exist. On NTFS, this is
  analogous to an Alternate Data Stream (ADS), and the default data stream for a file is just called
  $DATA. Zone.Identifier is commonly used by Windows to track contents downloaded from the Internet.
  An ADS is typically of the form: C:\path\to\filename.extension:some_fork_name, and some_fork_name
  is the value that should populate @code fork_name @endcode. @code filename.extension @endcode
  should populate @code file.name @endcode, and @code extension @endcode should populate @code
  file.extension @endcode. The full path, @code file.path @endcode, will include the fork name.
 */
static constexpr const char *kFileForkName = "file.fork_name";

/**
  Primary Group ID (GID) of the file.
 */
static constexpr const char *kFileGroupId = "file.group.id";

/**
  Primary group name of the file.
 */
static constexpr const char *kFileGroupName = "file.group.name";

/**
  Inode representing the file in the filesystem.
 */
static constexpr const char *kFileInode = "file.inode";

/**
  Mode of the file in octal representation.
 */
static constexpr const char *kFileMode = "file.mode";

/**
  Time when the file content was last modified, in ISO 8601 format.
 */
static constexpr const char *kFileModified = "file.modified";

/**
  Name of the file including the extension, without the directory.
 */
static constexpr const char *kFileName = "file.name";

/**
  The user ID (UID) or security identifier (SID) of the file owner.
 */
static constexpr const char *kFileOwnerId = "file.owner.id";

/**
  Username of the file owner.
 */
static constexpr const char *kFileOwnerName = "file.owner.name";

/**
  Full path to the file, including the file name. It should include the drive letter, when
  appropriate.
 */
static constexpr const char *kFilePath = "file.path";

/**
  File size in bytes.
 */
static constexpr const char *kFileSize = "file.size";

/**
  Path to the target of a symbolic link.
  <p>
  This attribute is only applicable to symbolic links.
 */
static constexpr const char *kFileSymbolicLinkTargetPath = "file.symbolic_link.target_path";

}  // namespace file
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
