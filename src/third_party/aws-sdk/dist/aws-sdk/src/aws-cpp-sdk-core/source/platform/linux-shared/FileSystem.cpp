/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/core/platform/FileSystem.h>

#include <aws/core/platform/Environment.h>
#include <aws/core/platform/Platform.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/UUID.h>

#include <unistd.h>
#include <pwd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <climits>

#include <cassert>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
namespace Aws
{
namespace FileSystem
{

static const char* FILE_SYSTEM_UTILS_LOG_TAG = "FileSystemUtils";

    class PosixDirectory : public Directory
    {
    public:
        PosixDirectory(const Aws::String& path, const Aws::String& relativePath) : Directory(path, relativePath), m_dir(nullptr)
        {
            m_dir = opendir(m_directoryEntry.path.c_str());
            AWS_LOGSTREAM_TRACE(FILE_SYSTEM_UTILS_LOG_TAG, "Entering directory " << m_directoryEntry.path);

            if(m_dir)
            {
                AWS_LOGSTREAM_TRACE(FILE_SYSTEM_UTILS_LOG_TAG, "Successfully opened directory " << m_directoryEntry.path);
                m_directoryEntry.fileType = FileType::Directory;
            }
            else
            {
                AWS_LOGSTREAM_ERROR(FILE_SYSTEM_UTILS_LOG_TAG, "Could not load directory " << m_directoryEntry.path << " with error code " << errno);
            }
        }

        ~PosixDirectory()
        {
            if (m_dir)
            {
                closedir(m_dir);
            }
        }

        operator bool() const override { return m_directoryEntry.operator bool() && m_dir != nullptr; }

        DirectoryEntry Next() override
        {
            assert(m_dir);
            DirectoryEntry entry;

            dirent* dirEntry;
            bool invalidEntry(true);

            while(invalidEntry)
            {
                if ((dirEntry = readdir(m_dir)))
                {
                    Aws::String entryName = dirEntry->d_name;
                    if(entryName != ".." && entryName != ".")
                    {
                        entry = ParseFileInfo(dirEntry, true);
                        invalidEntry = false;
                    }
                    else
                    {
                        AWS_LOGSTREAM_DEBUG(FILE_SYSTEM_UTILS_LOG_TAG, "skipping . or ..");
                    }
                }
                else
                {
                    break;
                }
            }

            return entry;
        }

    private:
        DirectoryEntry ParseFileInfo(dirent* dirEnt, bool computePath)
        {
            DirectoryEntry entry;

            if(computePath)
            {
                Aws::StringStream ss;
                ss << m_directoryEntry.path << PATH_DELIM << dirEnt->d_name;
                entry.path = ss.str();

                ss.str("");
                if(m_directoryEntry.relativePath.empty())
                {
                    ss << dirEnt->d_name;
                }
                else
                {
                    ss << m_directoryEntry.relativePath << PATH_DELIM << dirEnt->d_name;
                }
                entry.relativePath = ss.str();
            }
            else
            {
                entry.path = m_directoryEntry.path;
                entry.relativePath = m_directoryEntry.relativePath;
            }

            AWS_LOGSTREAM_TRACE(FILE_SYSTEM_UTILS_LOG_TAG, "Calling stat on path " << entry.path);

            struct stat dirInfo = {};
            if(!lstat(entry.path.c_str(), &dirInfo))
            {
               if(S_ISDIR(dirInfo.st_mode))
               {
                   AWS_LOGSTREAM_DEBUG(FILE_SYSTEM_UTILS_LOG_TAG, "type directory detected");
                   entry.fileType = FileType::Directory;
               }
               else if(S_ISLNK(dirInfo.st_mode))
               {
                   AWS_LOGSTREAM_DEBUG(FILE_SYSTEM_UTILS_LOG_TAG, "type symlink detected");
                   entry.fileType = FileType::Symlink;
               }
               else if(S_ISREG(dirInfo.st_mode))
               {
                   AWS_LOGSTREAM_DEBUG(FILE_SYSTEM_UTILS_LOG_TAG, "type file detected");
                   entry.fileType = FileType::File;
               }

               entry.fileSize = static_cast<unsigned long long>(dirInfo.st_size);
               AWS_LOGSTREAM_DEBUG(FILE_SYSTEM_UTILS_LOG_TAG, "file size detected as " << entry.fileSize);
            }
            else
            {
                AWS_LOGSTREAM_ERROR(FILE_SYSTEM_UTILS_LOG_TAG, "Failed to stat file path " << entry.path << " with error code " << errno);
            }

            return entry;
        }

        DIR* m_dir = nullptr;
    };

Aws::String GetHomeDirectory()
{
    static const char* HOME_DIR_ENV_VAR = "HOME";

    AWS_LOGSTREAM_TRACE(FILE_SYSTEM_UTILS_LOG_TAG, "Checking " << HOME_DIR_ENV_VAR << " for the home directory.");

    Aws::String homeDir = Aws::Environment::GetEnv(HOME_DIR_ENV_VAR);

    AWS_LOGSTREAM_DEBUG(FILE_SYSTEM_UTILS_LOG_TAG, "Environment value for variable " << HOME_DIR_ENV_VAR << " is " << homeDir);

    if(homeDir.empty())
    {
        AWS_LOGSTREAM_WARN(FILE_SYSTEM_UTILS_LOG_TAG, "Home dir not stored in environment, trying to fetch manually from the OS.");

        passwd pw;
        passwd *p_pw = nullptr;
        char pw_buffer[4096];
        getpwuid_r(getuid(), &pw, pw_buffer, sizeof(pw_buffer), &p_pw);
        if(p_pw && p_pw->pw_dir)
        {
            homeDir = p_pw->pw_dir;
        }

        AWS_LOGSTREAM_INFO(FILE_SYSTEM_UTILS_LOG_TAG, "Pulled " << homeDir << " as home directory from the OS.");
    }

    Aws::String retVal = homeDir.size() > 0 ? Aws::Utils::StringUtils::Trim(homeDir.c_str()) : "";
    if(!retVal.empty())
    {
        if(retVal.at(retVal.length() - 1) != PATH_DELIM)
        {
            AWS_LOGSTREAM_DEBUG(FILE_SYSTEM_UTILS_LOG_TAG, "Home directory is missing the final " << PATH_DELIM << " appending one to normalize");
            retVal += PATH_DELIM;
        }
    }

    AWS_LOGSTREAM_DEBUG(FILE_SYSTEM_UTILS_LOG_TAG, "Final Home Directory is " << retVal);

    return retVal;
}

bool CreateDirectoryIfNotExists(const char* path, bool createParentDirs)
{
    Aws::String directoryName = path;
    AWS_LOGSTREAM_INFO(FILE_SYSTEM_UTILS_LOG_TAG, "Creating directory " << directoryName);

    for (size_t i = (createParentDirs ? 0 : directoryName.size() - 1); i < directoryName.size(); i++)
    {
        // Create the parent directory if we find a delimiter and the delimiter is not the first char, or if this is the target directory.
        if (i != 0 && (directoryName[i] == FileSystem::PATH_DELIM || i == directoryName.size() - 1))
        {
            if (directoryName[i] == FileSystem::PATH_DELIM)
            {
                directoryName[i] = '\0';
            }
            int errorCode = mkdir(directoryName.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);
            if (errorCode != 0 && errno != EEXIST)
            {
                AWS_LOGSTREAM_ERROR(FILE_SYSTEM_UTILS_LOG_TAG, "Creation of directory " << directoryName.c_str() << " returned code: " << errno);
                return false;
            }
            AWS_LOGSTREAM_DEBUG(FILE_SYSTEM_UTILS_LOG_TAG, "Creation of directory " << directoryName.c_str() << " returned code: " << errno);
            directoryName[i] = FileSystem::PATH_DELIM;
        }
    }
    return true;
}

bool RemoveFileIfExists(const char* path)
{
    AWS_LOGSTREAM_INFO(FILE_SYSTEM_UTILS_LOG_TAG, "Deleting file: " << path);

    int errorCode = unlink(path);
    AWS_LOGSTREAM_DEBUG(FILE_SYSTEM_UTILS_LOG_TAG, "Deletion of file: " << path << " Returned error code: " << errno);
    return errorCode == 0 || errno == ENOENT;
}

bool RemoveDirectoryIfExists(const char* path)
{
    AWS_LOGSTREAM_INFO(FILE_SYSTEM_UTILS_LOG_TAG, "Deleting directory: " << path);
    int errorCode = rmdir(path);
    AWS_LOGSTREAM_DEBUG(FILE_SYSTEM_UTILS_LOG_TAG, "Deletion of directory: " << path << " Returned error code: " << errno);
    return errorCode == 0 || errno == ENOTDIR || errno == ENOENT;
}

bool RelocateFileOrDirectory(const char* from, const char* to)
{
    AWS_LOGSTREAM_INFO(FILE_SYSTEM_UTILS_LOG_TAG, "Moving file at " << from << " to " << to);

    int errorCode = std::rename(from, to);

    AWS_LOGSTREAM_DEBUG(FILE_SYSTEM_UTILS_LOG_TAG,  "The moving operation of file at " << from << " to " << to << " Returned error code of " << errno);
    return errorCode == 0;
}

Aws::String CreateTempFilePath()
{
    Aws::StringStream ss;
    auto dt = Aws::Utils::DateTime::Now();

    ss << dt.ToGmtString("%Y%m%dT%H%M%S") << dt.Millis() << Aws::String(Aws::Utils::UUID::PseudoRandomUUID());
    Aws::String tempFile(ss.str());

    AWS_LOGSTREAM_DEBUG(FILE_SYSTEM_UTILS_LOG_TAG, "CreateTempFilePath generated: " << tempFile);

    return tempFile;
}

Aws::String GetExecutableDirectory()
{
    char dest[PATH_MAX];
    memset(dest, 0, PATH_MAX);
#ifdef __APPLE__
    uint32_t destSize = PATH_MAX;
    if (_NSGetExecutablePath(dest, &destSize) == 0)
#else
    size_t destSize = PATH_MAX;
    if (readlink("/proc/self/exe", dest, destSize))
#endif
    {
        Aws::String executablePath(dest);
        auto lastSlash = executablePath.find_last_of('/');
        if(lastSlash != std::string::npos)
        {
            return executablePath.substr(0, lastSlash);
        }
    }
    return "./";
}

Aws::UniquePtr<Directory> OpenDirectory(const Aws::String& path, const Aws::String& relativePath)
{
    return Aws::MakeUnique<PosixDirectory>(FILE_SYSTEM_UTILS_LOG_TAG, path, relativePath);
}

} // namespace FileSystem
} // namespace Aws
