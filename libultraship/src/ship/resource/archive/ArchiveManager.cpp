#include "ship/resource/archive/ArchiveManager.h"

#include <algorithm>
#include <filesystem>
#include "spdlog/spdlog.h"

#include "ship/resource/archive/Archive.h"
#ifdef INCLUDE_MPQ_SUPPORT
#include "ship/resource/archive/OtrArchive.h"
#endif
#include "ship/resource/archive/O2rArchive.h"
#include "ship/resource/archive/FolderArchive.h"
#include "ship/utils/StringHelper.h"
#include "ship/utils/glob.h"
#include "ship/utils/StrHash64.h"

namespace Ship {
ArchiveManager::ArchiveManager() {
}

void ArchiveManager::Init(const std::vector<std::string>& archivePaths) {
    Init(archivePaths, {});
}

void ArchiveManager::Init(const std::vector<std::string>& archivePaths,
                          const std::unordered_set<uint32_t>& validGameVersions) {
    mValidGameVersions = validGameVersions;
    auto archives = GetArchiveListInPaths(archivePaths);
    for (const auto& archive : archives) {
        AddArchive(archive);
    }
}

ArchiveManager::~ArchiveManager() {
    SPDLOG_TRACE("destruct archive manager");
    SetArchives(nullptr);
}

bool ArchiveManager::IsLoaded() {
    return !mArchives.empty();
}

std::shared_ptr<File> ArchiveManager::LoadFile(const std::string& filePath) {
    if (filePath == "") {
        return nullptr;
    }

    return LoadFile(CRC64(filePath.c_str()));
}

const ArchiveManager::FileEntry* ArchiveManager::FindFile(uint64_t hash) const {
    auto it = std::lower_bound(mFiles.begin(), mFiles.end(), hash,
                               [](const auto& file, uint64_t h) { return file.first < h; });
    return it != mFiles.end() && it->first == hash ? &it->second : nullptr;
}

std::shared_ptr<File> ArchiveManager::LoadFile(uint64_t hash) {
    const FileEntry* file = FindFile(hash);
    if (file == nullptr) {
        return nullptr;
    }

    return mArchives[file->archive]->LoadFile(hash);
}

bool ArchiveManager::HasFile(const std::string& filePath) {
    return HasFile(CRC64(filePath.c_str()));
}

bool ArchiveManager::HasFile(uint64_t hash) {
    return FindFile(hash) != nullptr;
}

std::shared_ptr<Archive> ArchiveManager::GetArchiveFromFile(const std::string& filePath) {
    const FileEntry* file = FindFile(CRC64(filePath.c_str()));
    return file == nullptr ? nullptr : mArchives[file->archive];
}

int32_t ArchiveManager::GetFilePriority(const std::string& filePath) {
    const FileEntry* file = FindFile(CRC64(filePath.c_str()));
    if (file == nullptr) {
        return -1;
    }
    return mArchives[file->archive]->GetPriority();
}

std::shared_ptr<std::vector<std::string>> ArchiveManager::ListFiles(const std::string& searchMask) {
    std::list<std::string> includes = {};
    if (!searchMask.empty()) {
        includes.push_back(searchMask);
    }
    return ListFiles(includes, {});
}

std::shared_ptr<std::vector<std::string>> ArchiveManager::ListFiles(const std::list<std::string>& includes,
                                                                    const std::list<std::string>& excludes) {
    auto list = std::make_shared<std::vector<std::string>>();
    for (const auto& [hash, file] : mFiles) {
        const char* path = file.path;
        if (includes.empty() && excludes.empty()) {
            list->push_back(path);
            continue;
        }
        bool includeMatch = includes.empty();
        if (!includes.empty()) {
            for (const std::string& filter : includes) {
                if (glob_match(filter.c_str(), path)) {
                    includeMatch = true;
                    break;
                }
            }
        }
        bool excludeMatch = false;
        if (!excludes.empty()) {
            for (const std::string& filter : excludes) {
                if (glob_match(filter.c_str(), path)) {
                    excludeMatch = true;
                    break;
                }
            }
        }
        if (includeMatch && !excludeMatch) {
            list->push_back(path);
        }
    }
    return list;
}

std::shared_ptr<std::vector<std::string>> ArchiveManager::ListDirectories(const std::string& searchMask) {
    auto list = std::make_shared<std::vector<std::string>>();
    for (const std::string& dir : mDirectories) {
        if (glob_match(searchMask.c_str(), dir.c_str())) {
            list->push_back(dir);
        }
    }
    return list;
}

std::vector<uint32_t> ArchiveManager::GetGameVersions() {
    return mGameVersions;
}

void ArchiveManager::AddGameVersion(uint32_t newGameVersion) {
    mGameVersions.push_back(newGameVersion);
}

std::shared_ptr<std::vector<std::shared_ptr<Archive>>> ArchiveManager::GetArchives() {
    auto archives = std::make_shared<std::vector<std::shared_ptr<Archive>>>();
    for (const auto& archive : mArchives) {
        archives->push_back(archive);
    }
    return archives;
}

void ArchiveManager::ResetVirtualFileSystem() {
    // Store the original list of archives because we will clear it and re-add them.
    // The re-add will trigger the file virtual file system to get populated.
    auto archives = mArchives;
    mArchives.clear();
    mGameVersions.clear();
    mFiles.clear();
    mWrittenPaths.clear();
    for (const auto& archive : archives) {
        archive->Unload();
        archive->Load();
        AddArchive(archive);
    }
}

bool ArchiveManager::WriteFile(std::shared_ptr<Archive> archive, const std::string& filePath,
                               const std::vector<uint8_t>& data) {
    if (archive) {
        if (archive->WriteFile(filePath, data)) {
            auto hash = CRC64(filePath.c_str());
            auto it = std::find(mArchives.begin(), mArchives.end(), archive);
            if (it != mArchives.end()) {
                mWrittenPaths.push_back(filePath);
                const FileEntry written = { mWrittenPaths.back().c_str(),
                                            static_cast<uint32_t>(it - mArchives.begin()) };
                auto at = std::lower_bound(mFiles.begin(), mFiles.end(), hash,
                                           [](const auto& file, uint64_t h) { return file.first < h; });
                if (at != mFiles.end() && at->first == hash) {
                    at->second = written;
                } else {
                    mFiles.insert(at, { hash, written });
                }
            }
            return true; // Successfully wrote file
        }
    }
    return false; // Failed to write file
}

size_t ArchiveManager::RemoveArchive(const std::string& path) {
    for (size_t i = 0; i < mArchives.size(); i++) {
        if (path == mArchives[i]->GetPath()) {
            mArchives[i]->Unload();
            mArchives.erase(mArchives.begin() + i);
            ResetVirtualFileSystem();
            return 1;
        }
    }

    return 0;
}

size_t ArchiveManager::RemoveArchive(std::shared_ptr<Archive> archive) {
    return RemoveArchive(archive->GetPath());
}

void ArchiveManager::SetArchives(std::shared_ptr<std::vector<std::shared_ptr<Archive>>> archives) {
    mArchives.clear();

    if (archives) {
        for (const auto& archive : *archives) {
            mArchives.push_back(archive);
        }
    }

    ResetVirtualFileSystem();
}

std::string ArchiveManager::HashToString(uint64_t hash) const {
    const char* path = HashToCString(hash);
    return path != nullptr ? std::string(path) : std::string();
}

const char* ArchiveManager::HashToCString(uint64_t hash) const {
    const FileEntry* file = FindFile(hash);
    return file != nullptr ? file->path : nullptr;
}

// std::filesystem::absolute; on the Xbox (whose libc++ filesystem is POSIX's) a path that starts with a
// drive letter, "D:/x", is already absolute, where absolute() would put the current directory before it
static std::string AbsolutePath(const std::filesystem::path& path) {
#ifdef LUS_XBOX
    const std::string s = path.string();
    if (s.size() >= 2 && s[1] == ':') {
        return s;
    }
#endif
    return std::filesystem::absolute(path).string();
}

std::vector<std::string> ArchiveManager::GetArchiveListInPaths(const std::vector<std::string>& archivePaths) {
    std::vector<std::string> fileList = {};

    for (const auto& archivePath : archivePaths) {
        if (archivePath.length() > 0) {
            if (std::filesystem::is_directory(archivePath)) {
                bool hasAssetFiles = false;
                for (const auto& p : std::filesystem::directory_iterator(archivePath)) {
                    if (StringHelper::IEquals(p.path().extension().string(), ".otr") ||
                        StringHelper::IEquals(p.path().extension().string(), ".zip") ||
                        StringHelper::IEquals(p.path().extension().string(), ".mpq") ||
                        StringHelper::IEquals(p.path().extension().string(), ".o2r")) {
                        fileList.push_back(AbsolutePath(p.path()));
                        hasAssetFiles = true;
                    }
                }

                if (!hasAssetFiles) {
                    fileList.push_back(AbsolutePath(archivePath));
                }
            } else if (std::filesystem::is_regular_file(archivePath)) {
                fileList.push_back(AbsolutePath(archivePath));
            } else {
                SPDLOG_WARN("The archive at path {} does not exist", AbsolutePath(archivePath));
            }
        } else {
            SPDLOG_WARN("No archive path supplied");
        }
    }

    return fileList;
}

std::shared_ptr<Archive> ArchiveManager::AddArchive(const std::string& archivePath) {
    const std::filesystem::path path = archivePath;
    const std::string extension = path.extension().string();
    std::shared_ptr<Archive> archive = nullptr;

    SPDLOG_INFO("Reading archive: {}", path.string());

    if (StringHelper::IEquals(extension, ".o2r") || StringHelper::IEquals(extension, ".zip")) {
        archive = dynamic_pointer_cast<Archive>(std::make_shared<O2rArchive>(archivePath));
#ifdef INCLUDE_MPQ_SUPPORT
    } else if (StringHelper::IEquals(extension, ".otr") || StringHelper::IEquals(extension, ".mpq")) {
        archive = dynamic_pointer_cast<Archive>(std::make_shared<OtrArchive>(archivePath));
#endif
    } else if (StringHelper::IEquals(extension, "")) {
        archive = dynamic_pointer_cast<Archive>(std::make_shared<FolderArchive>(archivePath));
    } else {
        // Not recognized file extension, trying with o2r
        SPDLOG_WARN("File extension \"{}\" not recognized, trying to create an o2r archive.", extension);
        archive = std::make_shared<O2rArchive>(archivePath);
    }

    archive->Load();
    return AddArchive(archive);
}

std::shared_ptr<Archive> ArchiveManager::AddArchive(std::shared_ptr<Archive> archive) {
    if (!archive->IsLoaded()) {
        SPDLOG_WARN("Attempting to add unloaded Archive at {} to Archive Manager", archive->GetPath());
        return nullptr;
    }

    if (!mValidGameVersions.empty() && !mValidGameVersions.contains(archive->GetGameVersion())) {
        SPDLOG_WARN("Attempting to add Archive at {} with invalid Game Version {} to Archive Manager",
                    archive->GetPath(), archive->GetGameVersion());
        return nullptr;
    }

    SPDLOG_INFO("Adding Archive {} to Archive Manager", archive->GetPath());

    mArchives.push_back(archive);
    // Index in mArchives is the load-order priority (last added = highest, wins conflicts).
    archive->SetPriority(static_cast<int32_t>(mArchives.size() - 1));
    if (archive->HasGameVersion()) {
        mGameVersions.push_back(archive->GetGameVersion());
    }
    const auto fileList = archive->ListFiles();
    const uint32_t index = static_cast<uint32_t>(mArchives.size() - 1);
    mFiles.reserve(mFiles.size() + fileList->size());
    for (auto& [hash, filename] : *fileList.get()) {
        mFiles.push_back({ hash, FileEntry{ filename, index } });

        const char* lastSlash = strrchr(filename, '/');
        if (lastSlash != nullptr) {
            mDirectories.emplace(filename, lastSlash - filename);
        }
    }
    // by hash, and a file in several archives kept from the last added (the highest priority)
    std::sort(mFiles.begin(), mFiles.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second.archive < b.second.archive;
    });
    size_t kept = 0;
    for (size_t i = 0; i < mFiles.size(); i++) {
        if (i + 1 == mFiles.size() || mFiles[i + 1].first != mFiles[i].first) {
            mFiles[kept++] = mFiles[i];
        }
    }
    mFiles.resize(kept);
#ifdef LUS_XBOX
    archive->DropHashIndex(); // merged: mFiles has it all
#endif
    return archive;
}

bool ArchiveManager::IsGameVersionValid(uint32_t gameVersion) {
    return mValidGameVersions.empty() || mValidGameVersions.contains(gameVersion);
}

#ifdef ENABLE_SCRIPTING
void ArchiveManager::SetUntrustedArchiveHandler(const UntrustedArchiveHandler& handler) {
    mUntrustedArchiveHandler = handler;
}

UntrustedArchiveHandler ArchiveManager::GetUntrustedArchiveHandler() const {
    return mUntrustedArchiveHandler;
}
#endif

} // namespace Ship
