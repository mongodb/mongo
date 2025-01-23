/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/platform/FileSystem.h>
#include <aws/core/utils/memory/stl/AWSQueue.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/StringUtils.h>
#include <fstream>
#include <cassert>

namespace Aws
{
    namespace FileSystem
    {
        Aws::String Join(const Aws::String& leftSegment, const Aws::String& rightSegment)
        {
            return Join(PATH_DELIM, leftSegment, rightSegment);
        }

        Aws::String Join(char delimiter, const Aws::String& leftSegment, const Aws::String& rightSegment)
        {
            Aws::StringStream ss;

            if (!leftSegment.empty())
            {
                if (leftSegment.back() == delimiter)
                {
                    ss << leftSegment.substr(0, leftSegment.length() - 1);
                }
                else
                {
                    ss << leftSegment;
                }
            }

            ss << delimiter;

            if (!rightSegment.empty())
            {
                if (rightSegment.front() == delimiter)
                {
                    ss << rightSegment.substr(1);
                }
                else
                {
                    ss << rightSegment;
                }
            }

            return ss.str();
        }

        bool DeepCopyDirectory(const char* from, const char* to)
        {
            if (!from || !to) return false;

            DirectoryTree fromDir(from);

            if (!fromDir) return false;

            CreateDirectoryIfNotExists(to);
            DirectoryTree toDir(to);

            if (!toDir) return false;

            bool success(true);            

            auto visitor = [to,&success](const DirectoryTree*, const DirectoryEntry& entry)
            {
                auto newPath = Aws::FileSystem::Join(to, entry.relativePath);

                if (entry.fileType == Aws::FileSystem::FileType::File)
                {
                    Aws::OFStream copyOutStream(newPath.c_str());
                    Aws::IFStream originalStream(entry.path.c_str());

                    if(!copyOutStream.good() || !originalStream.good())
                    {
                        success = false; 
                        return false;
                    }

                    std::copy(std::istreambuf_iterator<char>(originalStream),
                        std::istreambuf_iterator<char>(), std::ostreambuf_iterator<char>(copyOutStream));
                }
                else if (entry.fileType == Aws::FileSystem::FileType::Directory)
                {
                    success = CreateDirectoryIfNotExists(newPath.c_str());
                    return success;
                }

                return success;
            };

            fromDir.TraverseDepthFirst(visitor);
            return success;
        }

        bool DeepDeleteDirectory(const char* toDelete)
        {
            bool success(true);

            //scope this to a new stack frame, because we won't be able to delete the root directory
            //unless the directory handle has closed.
            {
                DirectoryTree delDir(toDelete);

                if (!delDir) return false;           

                auto visitor = [&success](const DirectoryTree*, const DirectoryEntry& entry)
                {
                    if (entry.fileType == FileType::File)
                    {
                        success = RemoveFileIfExists(entry.path.c_str());
                    }
                    else
                    {
                        success = RemoveDirectoryIfExists(entry.path.c_str());
                    }

                    return success;
                };

                delDir.TraverseDepthFirst(visitor, true);
            }

            if (success)
            {
                success = RemoveDirectoryIfExists(toDelete);
            }

            return success;
        }

        Directory::Directory(const Aws::String& path, const Aws::String& relativePath)
        {
            auto trimmedPath = Utils::StringUtils::Trim(path.c_str());
            auto trimmedRelativePath = Utils::StringUtils::Trim(relativePath.c_str());

            if (!trimmedPath.empty() && trimmedPath[trimmedPath.length() - 1] == PATH_DELIM)
            {
                m_directoryEntry.path = trimmedPath.substr(0, trimmedPath.length() - 1);
            }
            else
            {
                m_directoryEntry.path = trimmedPath;
            }    

            if (!trimmedRelativePath.empty() && trimmedRelativePath[trimmedRelativePath.length() - 1] == PATH_DELIM)
            {
                m_directoryEntry.relativePath = trimmedRelativePath.substr(0, trimmedRelativePath.length() - 1);
            }
            else
            {
                m_directoryEntry.relativePath = trimmedRelativePath;
            }          
        }        

        Aws::UniquePtr<Directory> Directory::Descend(const DirectoryEntry& directoryEntry)
        {
            assert(directoryEntry.fileType != FileType::File);
            return OpenDirectory(directoryEntry.path, directoryEntry.relativePath);
        }

        Aws::Vector<Aws::String> Directory::GetAllFilePathsInDirectory(const Aws::String& path)
        {
            Aws::FileSystem::DirectoryTree tree(path);
            Aws::Vector<Aws::String> filesVector;
            auto visitor = [&](const Aws::FileSystem::DirectoryTree*, const Aws::FileSystem::DirectoryEntry& entry) 
            { 
                if (entry.fileType == Aws::FileSystem::FileType::File)
                {
                    filesVector.push_back(entry.path);
                }
                return true;
            };
            tree.TraverseBreadthFirst(visitor);
            return filesVector;
        }

        DirectoryTree::DirectoryTree(const Aws::String& path)
        {
            m_dir = OpenDirectory(path);
        }

        DirectoryTree::operator bool() const
        {
            return m_dir->operator bool();
        }

        bool DirectoryTree::operator==(DirectoryTree& other)
        {
            return Diff(other).size() == 0;
        }

        bool DirectoryTree::operator==(const Aws::String& path)
        {
            return *this == DirectoryTree(path);
        }

        Aws::Map<Aws::String, DirectoryEntry> DirectoryTree::Diff(DirectoryTree& other)
        {
            Aws::Map<Aws::String, DirectoryEntry> thisEntries;
            auto thisTraversal = [&thisEntries](const DirectoryTree*, const DirectoryEntry& entry)
            {
                thisEntries[entry.relativePath] = entry;
                return true;
            };

            Aws::Map<Aws::String, DirectoryEntry> otherEntries;
            auto otherTraversal = [&thisEntries, &otherEntries](const DirectoryTree*, const DirectoryEntry& entry)
            {
                auto thisEntry = thisEntries.find(entry.relativePath);
                if (thisEntry != thisEntries.end())
                {
                    thisEntries.erase(entry.relativePath);
                }
                else
                {
                    otherEntries[entry.relativePath] = entry;
                }

                return true;
            };

            TraverseDepthFirst(thisTraversal);
            other.TraverseDepthFirst(otherTraversal);

            thisEntries.insert(otherEntries.begin(), otherEntries.end());
            return thisEntries;
        }

        void DirectoryTree::TraverseDepthFirst(const DirectoryEntryVisitor& visitor, bool postOrderTraversal)
        {
            TraverseDepthFirst(*m_dir, visitor, postOrderTraversal);
            m_dir = OpenDirectory(m_dir->GetPath());
        }

        void DirectoryTree::TraverseBreadthFirst(const DirectoryEntryVisitor& visitor)
        {
            TraverseBreadthFirst(*m_dir, visitor);
            m_dir = OpenDirectory(m_dir->GetPath());
        }
         
        void DirectoryTree::TraverseBreadthFirst(Directory& dir, const DirectoryEntryVisitor& visitor)
        {
            if (!dir)
            {
                return;
            }

            Aws::Queue<DirectoryEntry> queue;
            while (DirectoryEntry&& entry = dir.Next())
            {
                queue.push(std::move(entry));
            }

            while (queue.size() > 0)
            {
                auto entry = queue.front();
                queue.pop();
                if(visitor(this, entry))               
                {
                    if(entry.fileType == FileType::Directory)
                    {
                        auto currentDir = dir.Descend(entry);

                        while (DirectoryEntry&& dirEntry = currentDir->Next())
                        {
                            queue.push(std::move(dirEntry));
                        }
                    }
                }
                else
                {
                    return;
                }
            }
        }

        bool DirectoryTree::TraverseDepthFirst(Directory& dir, const DirectoryEntryVisitor& visitor, bool postOrder)
        {
            if (!dir)
            {
                return true;
            }

            bool exitTraversal(false);
            DirectoryEntry entry;

            while ((entry = dir.Next()) && !exitTraversal)
            {
                if(!postOrder)
                {
                    if(!visitor(this, entry))
                    {                    
                        return false;
                    }
                }

                if (entry.fileType == FileType::Directory)
                {
                    auto subDir = dir.Descend(entry);
                    exitTraversal = !TraverseDepthFirst(*subDir, visitor, postOrder);
                } 
                
                if (postOrder)
                {
                    if (!visitor(this, entry))
                    {
                        return false;
                    }
                }
            }

            return !exitTraversal;
        }       

    }
}
