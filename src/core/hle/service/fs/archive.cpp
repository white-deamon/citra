// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <memory>
#include <unordered_map>

#include <boost/container/flat_map.hpp>

#include "common/common_types.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/make_unique.h"
#include "common/math_util.h"

#include "core/file_sys/archive_backend.h"
#include "core/file_sys/archive_extsavedata.h"
#include "core/file_sys/archive_romfs.h"
#include "core/file_sys/archive_savedata.h"
#include "core/file_sys/archive_savedatacheck.h"
#include "core/file_sys/archive_sdmc.h"
#include "core/file_sys/archive_systemsavedata.h"
#include "core/file_sys/directory_backend.h"
#include "core/hle/service/service.h"
#include "core/hle/service/fs/archive.h"
#include "core/hle/service/fs/fs_user.h"
#include "core/hle/result.h"

// Specializes std::hash for ArchiveIdCode, so that we can use it in std::unordered_map.
// Workaroung for libstdc++ bug: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=60970
namespace std {
    template <>
    struct hash<Service::FS::ArchiveIdCode> {
        typedef Service::FS::ArchiveIdCode argument_type;
        typedef std::size_t result_type;

        result_type operator()(const argument_type& id_code) const {
            typedef std::underlying_type<argument_type>::type Type;
            return std::hash<Type>()(static_cast<Type>(id_code));
        }
    };
}

/// TODO(Subv): Confirm length of these strings
const std::string SYSTEM_ID = "00000000000000000000000000000000";
const std::string SDCARD_ID = "00000000000000000000000000000000";

namespace Service {
namespace FS {

// TODO: Verify code
/// Returned when a function is passed an invalid handle.
const ResultCode ERR_INVALID_HANDLE(ErrorDescription::InvalidHandle, ErrorModule::FS,
        ErrorSummary::InvalidArgument, ErrorLevel::Permanent);

// Command to access archive file
enum class FileCommand : u32 {
    Dummy1          = 0x000100C6,
    Control         = 0x040100C4,
    OpenSubFile     = 0x08010100,
    Read            = 0x080200C2,
    Write           = 0x08030102,
    GetSize         = 0x08040000,
    SetSize         = 0x08050080,
    GetAttributes   = 0x08060000,
    SetAttributes   = 0x08070040,
    Close           = 0x08080000,
    Flush           = 0x08090000,
    SetPriority     = 0x080A0040,
    GetPriority     = 0x080B0000,
    OpenLinkFile    = 0x080C0000,
};

// Command to access directory
enum class DirectoryCommand : u32 {
    Dummy1          = 0x000100C6,
    Control         = 0x040100C4,
    Read            = 0x08010042,
    Close           = 0x08020000,
};

File::File(std::unique_ptr<FileSys::FileBackend>&& backend, const FileSys::Path & path)
    : path(path), priority(0), backend(std::move(backend)) {}

File::~File() {}

ResultVal<bool> File::SyncRequest() {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    FileCommand cmd = static_cast<FileCommand>(cmd_buff[0]);
    switch (cmd) {

        // Read from file...
        case FileCommand::Read:
        {
            u64 offset = cmd_buff[1] | ((u64)cmd_buff[2]) << 32;
            u32 length = cmd_buff[3];
            u32 address = cmd_buff[5];
            LOG_TRACE(Service_FS, "Read %s %s: offset=0x%llx length=%d address=0x%x",
                      GetTypeName().c_str(), GetName().c_str(), offset, length, address);
            cmd_buff[2] = static_cast<u32>(backend->Read(offset, length, Memory::GetPointer(address)));
            break;
        }

        // Write to file...
        case FileCommand::Write:
        {
            u64 offset = cmd_buff[1] | ((u64)cmd_buff[2]) << 32;
            u32 length = cmd_buff[3];
            u32 flush = cmd_buff[4];
            u32 address = cmd_buff[6];
            LOG_TRACE(Service_FS, "Write %s %s: offset=0x%llx length=%d address=0x%x, flush=0x%x",
                      GetTypeName().c_str(), GetName().c_str(), offset, length, address, flush);
            cmd_buff[2] = static_cast<u32>(backend->Write(offset, length, flush, Memory::GetPointer(address)));
            break;
        }

        case FileCommand::GetSize:
        {
            LOG_TRACE(Service_FS, "GetSize %s %s", GetTypeName().c_str(), GetName().c_str());
            u64 size = backend->GetSize();
            cmd_buff[2] = (u32)size;
            cmd_buff[3] = size >> 32;
            break;
        }

        case FileCommand::SetSize:
        {
            u64 size = cmd_buff[1] | ((u64)cmd_buff[2] << 32);
            LOG_TRACE(Service_FS, "SetSize %s %s size=%llu",
                GetTypeName().c_str(), GetName().c_str(), size);
            backend->SetSize(size);
            break;
        }

        case FileCommand::Close:
        {
            LOG_TRACE(Service_FS, "Close %s %s", GetTypeName().c_str(), GetName().c_str());
            backend->Close();
            break;
        }

        case FileCommand::Flush:
        {
            LOG_TRACE(Service_FS, "Flush");
            backend->Flush();
            break;
        }

        case FileCommand::OpenLinkFile:
        {
            LOG_WARNING(Service_FS, "(STUBBED) File command OpenLinkFile %s", GetName().c_str());
            cmd_buff[3] = Kernel::g_handle_table.Create(this).ValueOr(INVALID_HANDLE);
            break;
        }

        case FileCommand::SetPriority:
        {
            priority = cmd_buff[1];
            LOG_TRACE(Service_FS, "SetPriority %u", priority);
            break;
        }

        case FileCommand::GetPriority:
        {
            cmd_buff[2] = priority;
            LOG_TRACE(Service_FS, "GetPriority");
            break;
        }

        // Unknown command...
        default:
            LOG_ERROR(Service_FS, "Unknown command=0x%08X!", cmd);
            ResultCode error = UnimplementedFunction(ErrorModule::FS);
            cmd_buff[1] = error.raw; // TODO(Link Mauve): use the correct error code for that.
            return error;
    }
    cmd_buff[1] = RESULT_SUCCESS.raw; // No error
    return MakeResult<bool>(false);
}

Directory::Directory(std::unique_ptr<FileSys::DirectoryBackend>&& backend, const FileSys::Path & path)
    : path(path), backend(std::move(backend)) {}

Directory::~Directory() {}

ResultVal<bool> Directory::SyncRequest() {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    DirectoryCommand cmd = static_cast<DirectoryCommand>(cmd_buff[0]);
    switch (cmd) {

        // Read from directory...
        case DirectoryCommand::Read:
        {
            u32 count = cmd_buff[1];
            u32 address = cmd_buff[3];
            auto entries = reinterpret_cast<FileSys::Entry*>(Memory::GetPointer(address));
            LOG_TRACE(Service_FS, "Read %s %s: count=%d",
                GetTypeName().c_str(), GetName().c_str(), count);

            // Number of entries actually read
            cmd_buff[2] = backend->Read(count, entries);
            break;
        }

        case DirectoryCommand::Close:
        {
            LOG_TRACE(Service_FS, "Close %s %s", GetTypeName().c_str(), GetName().c_str());
            backend->Close();
            break;
        }

        // Unknown command...
        default:
            LOG_ERROR(Service_FS, "Unknown command=0x%08X!", cmd);
            ResultCode error = UnimplementedFunction(ErrorModule::FS);
            cmd_buff[1] = error.raw; // TODO(Link Mauve): use the correct error code for that.
            return MakeResult<bool>(false);
    }
    cmd_buff[1] = RESULT_SUCCESS.raw; // No error
    return MakeResult<bool>(false);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

using FileSys::ArchiveBackend;
using FileSys::ArchiveFactory;

/**
 * Map of registered archives, identified by id code. Once an archive is registered here, it is
 * never removed until the FS service is shut down.
 */
static boost::container::flat_map<ArchiveIdCode, std::unique_ptr<ArchiveFactory>> id_code_map;

/**
 * Map of active archive handles. Values are pointers to the archives in `idcode_map`.
 */
static std::unordered_map<ArchiveHandle, std::unique_ptr<ArchiveBackend>> handle_map;
static ArchiveHandle next_handle;

static ArchiveBackend* GetArchive(ArchiveHandle handle) {
    auto itr = handle_map.find(handle);
    return (itr == handle_map.end()) ? nullptr : itr->second.get();
}

ResultVal<ArchiveHandle> OpenArchive(ArchiveIdCode id_code, FileSys::Path& archive_path) {
    LOG_TRACE(Service_FS, "Opening archive with id code 0x%08X", id_code);

    auto itr = id_code_map.find(id_code);
    if (itr == id_code_map.end()) {
        // TODO: Verify error against hardware
        return ResultCode(ErrorDescription::NotFound, ErrorModule::FS,
                          ErrorSummary::NotFound, ErrorLevel::Permanent);
    }

    CASCADE_RESULT(std::unique_ptr<ArchiveBackend> res, itr->second->Open(archive_path));

    // This should never even happen in the first place with 64-bit handles,
    while (handle_map.count(next_handle) != 0) {
        ++next_handle;
    }
    handle_map.emplace(next_handle, std::move(res));
    return MakeResult<ArchiveHandle>(next_handle++);
}

ResultCode CloseArchive(ArchiveHandle handle) {
    if (handle_map.erase(handle) == 0)
        return ERR_INVALID_HANDLE;
    else
        return RESULT_SUCCESS;
}

// TODO(yuriks): This might be what the fs:REG service is for. See the Register/Unregister calls in
// http://3dbrew.org/wiki/Filesystem_services#ProgramRegistry_service_.22fs:REG.22
ResultCode RegisterArchiveType(std::unique_ptr<FileSys::ArchiveFactory>&& factory, ArchiveIdCode id_code) {
    auto result = id_code_map.emplace(id_code, std::move(factory));

    bool inserted = result.second;
    ASSERT_MSG(inserted, "Tried to register more than one archive with same id code");

    auto& archive = result.first->second;
    LOG_DEBUG(Service_FS, "Registered archive %s with id code 0x%08X", archive->GetName().c_str(), id_code);
    return RESULT_SUCCESS;
}

ResultVal<Kernel::SharedPtr<File>> OpenFileFromArchive(ArchiveHandle archive_handle,
        const FileSys::Path& path, const FileSys::Mode mode) {
    ArchiveBackend* archive = GetArchive(archive_handle);
    if (archive == nullptr)
        return ERR_INVALID_HANDLE;

    std::unique_ptr<FileSys::FileBackend> backend = archive->OpenFile(path, mode);
    if (backend == nullptr) {
        return ResultCode(ErrorDescription::FS_NotFound, ErrorModule::FS,
                          ErrorSummary::NotFound, ErrorLevel::Status);
    }

    auto file = Kernel::SharedPtr<File>(new File(std::move(backend), path));
    return MakeResult<Kernel::SharedPtr<File>>(std::move(file));
}

ResultCode DeleteFileFromArchive(ArchiveHandle archive_handle, const FileSys::Path& path) {
    ArchiveBackend* archive = GetArchive(archive_handle);
    if (archive == nullptr)
        return ERR_INVALID_HANDLE;

    if (archive->DeleteFile(path))
        return RESULT_SUCCESS;
    return ResultCode(ErrorDescription::NoData, ErrorModule::FS, // TODO: verify description
                      ErrorSummary::Canceled, ErrorLevel::Status);
}

ResultCode RenameFileBetweenArchives(ArchiveHandle src_archive_handle, const FileSys::Path& src_path,
                                     ArchiveHandle dest_archive_handle, const FileSys::Path& dest_path) {
    ArchiveBackend* src_archive = GetArchive(src_archive_handle);
    ArchiveBackend* dest_archive = GetArchive(dest_archive_handle);
    if (src_archive == nullptr || dest_archive == nullptr)
        return ERR_INVALID_HANDLE;

    if (src_archive == dest_archive) {
        if (src_archive->RenameFile(src_path, dest_path))
            return RESULT_SUCCESS;
    } else {
        // TODO: Implement renaming across archives
        return UnimplementedFunction(ErrorModule::FS);
    }

    // TODO(yuriks): This code probably isn't right, it'll return a Status even if the file didn't
    // exist or similar. Verify.
    return ResultCode(ErrorDescription::NoData, ErrorModule::FS, // TODO: verify description
                      ErrorSummary::NothingHappened, ErrorLevel::Status);
}

ResultCode DeleteDirectoryFromArchive(ArchiveHandle archive_handle, const FileSys::Path& path) {
    ArchiveBackend* archive = GetArchive(archive_handle);
    if (archive == nullptr)
        return ERR_INVALID_HANDLE;

    if (archive->DeleteDirectory(path))
        return RESULT_SUCCESS;
    return ResultCode(ErrorDescription::NoData, ErrorModule::FS, // TODO: verify description
                      ErrorSummary::Canceled, ErrorLevel::Status);
}

ResultCode CreateFileInArchive(ArchiveHandle archive_handle, const FileSys::Path& path, u32 file_size) {
    ArchiveBackend* archive = GetArchive(archive_handle);
    if (archive == nullptr)
        return ERR_INVALID_HANDLE;

    return archive->CreateFile(path, file_size);
}

ResultCode CreateDirectoryFromArchive(ArchiveHandle archive_handle, const FileSys::Path& path) {
    ArchiveBackend* archive = GetArchive(archive_handle);
    if (archive == nullptr)
        return ERR_INVALID_HANDLE;

    if (archive->CreateDirectory(path))
        return RESULT_SUCCESS;
    return ResultCode(ErrorDescription::NoData, ErrorModule::FS, // TODO: verify description
                      ErrorSummary::Canceled, ErrorLevel::Status);
}

ResultCode RenameDirectoryBetweenArchives(ArchiveHandle src_archive_handle, const FileSys::Path& src_path,
                                          ArchiveHandle dest_archive_handle, const FileSys::Path& dest_path) {
    ArchiveBackend* src_archive = GetArchive(src_archive_handle);
    ArchiveBackend* dest_archive = GetArchive(dest_archive_handle);
    if (src_archive == nullptr || dest_archive == nullptr)
        return ERR_INVALID_HANDLE;

    if (src_archive == dest_archive) {
        if (src_archive->RenameDirectory(src_path, dest_path))
            return RESULT_SUCCESS;
    } else {
        // TODO: Implement renaming across archives
        return UnimplementedFunction(ErrorModule::FS);
    }

    // TODO(yuriks): This code probably isn't right, it'll return a Status even if the file didn't
    // exist or similar. Verify.
    return ResultCode(ErrorDescription::NoData, ErrorModule::FS, // TODO: verify description
                      ErrorSummary::NothingHappened, ErrorLevel::Status);
}

ResultVal<Kernel::SharedPtr<Directory>> OpenDirectoryFromArchive(ArchiveHandle archive_handle,
        const FileSys::Path& path) {
    ArchiveBackend* archive = GetArchive(archive_handle);
    if (archive == nullptr)
        return ERR_INVALID_HANDLE;

    std::unique_ptr<FileSys::DirectoryBackend> backend = archive->OpenDirectory(path);
    if (backend == nullptr) {
        return ResultCode(ErrorDescription::NotFound, ErrorModule::FS,
                          ErrorSummary::NotFound, ErrorLevel::Permanent);
    }

    auto directory = Kernel::SharedPtr<Directory>(new Directory(std::move(backend), path));
    return MakeResult<Kernel::SharedPtr<Directory>>(std::move(directory));
}

ResultCode FormatArchive(ArchiveIdCode id_code, const FileSys::Path& path) {
    auto archive_itr = id_code_map.find(id_code);
    if (archive_itr == id_code_map.end()) {
        return UnimplementedFunction(ErrorModule::FS); // TODO(Subv): Find the right error
    }

    return archive_itr->second->Format(path);
}

ResultCode CreateExtSaveData(MediaType media_type, u32 high, u32 low) {
    // Construct the binary path to the archive first
    FileSys::Path path = FileSys::ConstructExtDataBinaryPath(static_cast<u32>(media_type), high, low);

    std::string media_type_directory;
    if (media_type == MediaType::NAND) {
        media_type_directory = FileUtil::GetUserPath(D_NAND_IDX);
    } else if (media_type == MediaType::SDMC) {
        media_type_directory = FileUtil::GetUserPath(D_SDMC_IDX);
    } else {
        LOG_ERROR(Service_FS, "Unsupported media type %u", media_type);
        return ResultCode(-1); // TODO(Subv): Find the right error code
    }

    std::string base_path = FileSys::GetExtDataContainerPath(media_type_directory, media_type == MediaType::NAND);
    std::string extsavedata_path = FileSys::GetExtSaveDataPath(base_path, path);
    if (!FileUtil::CreateFullPath(extsavedata_path))
        return ResultCode(-1); // TODO(Subv): Find the right error code
    return RESULT_SUCCESS;
}

ResultCode DeleteExtSaveData(MediaType media_type, u32 high, u32 low) {
    // Construct the binary path to the archive first
    FileSys::Path path = FileSys::ConstructExtDataBinaryPath(static_cast<u32>(media_type), high, low);

    std::string media_type_directory;
    if (media_type == MediaType::NAND) {
        media_type_directory = FileUtil::GetUserPath(D_NAND_IDX);
    } else if (media_type == MediaType::SDMC) {
        media_type_directory = FileUtil::GetUserPath(D_SDMC_IDX);
    } else {
        LOG_ERROR(Service_FS, "Unsupported media type %u", media_type);
        return ResultCode(-1); // TODO(Subv): Find the right error code
    }

    std::string base_path = FileSys::GetExtDataContainerPath(media_type_directory, media_type == MediaType::NAND);
    std::string extsavedata_path = FileSys::GetExtSaveDataPath(base_path, path);
    if (!FileUtil::DeleteDirRecursively(extsavedata_path))
        return ResultCode(-1); // TODO(Subv): Find the right error code
    return RESULT_SUCCESS;
}

ResultCode DeleteSystemSaveData(u32 high, u32 low) {
    // Construct the binary path to the archive first
    FileSys::Path path = FileSys::ConstructSystemSaveDataBinaryPath(high, low);

    std::string nand_directory = FileUtil::GetUserPath(D_NAND_IDX);
    std::string base_path = FileSys::GetSystemSaveDataContainerPath(nand_directory);
    std::string systemsavedata_path = FileSys::GetSystemSaveDataPath(base_path, path);
    if (!FileUtil::DeleteDirRecursively(systemsavedata_path))
        return ResultCode(-1); // TODO(Subv): Find the right error code
    return RESULT_SUCCESS;
}

ResultCode CreateSystemSaveData(u32 high, u32 low) {
    // Construct the binary path to the archive first
    FileSys::Path path = FileSys::ConstructSystemSaveDataBinaryPath(high, low);

    std::string nand_directory = FileUtil::GetUserPath(D_NAND_IDX);
    std::string base_path = FileSys::GetSystemSaveDataContainerPath(nand_directory);
    std::string systemsavedata_path = FileSys::GetSystemSaveDataPath(base_path, path);
    if (!FileUtil::CreateFullPath(systemsavedata_path))
        return ResultCode(-1); // TODO(Subv): Find the right error code
    return RESULT_SUCCESS;
}

/// Initialize archives
void ArchiveInit() {
    next_handle = 1;

    AddService(new FS::Interface);

    // TODO(Subv): Add the other archive types (see here for the known types:
    // http://3dbrew.org/wiki/FS:OpenArchive#Archive_idcodes).

    std::string sdmc_directory = FileUtil::GetUserPath(D_SDMC_IDX);
    std::string nand_directory = FileUtil::GetUserPath(D_NAND_IDX);
    auto sdmc_factory = Common::make_unique<FileSys::ArchiveFactory_SDMC>(sdmc_directory);
    if (sdmc_factory->Initialize())
        RegisterArchiveType(std::move(sdmc_factory), ArchiveIdCode::SDMC);
    else
        LOG_ERROR(Service_FS, "Can't instantiate SDMC archive with path %s", sdmc_directory.c_str());

    // Create the SaveData archive
    auto savedata_factory = Common::make_unique<FileSys::ArchiveFactory_SaveData>(sdmc_directory);
    RegisterArchiveType(std::move(savedata_factory), ArchiveIdCode::SaveData);

    auto extsavedata_factory = Common::make_unique<FileSys::ArchiveFactory_ExtSaveData>(sdmc_directory, false);
    if (extsavedata_factory->Initialize())
        RegisterArchiveType(std::move(extsavedata_factory), ArchiveIdCode::ExtSaveData);
    else
        LOG_ERROR(Service_FS, "Can't instantiate ExtSaveData archive with path %s", extsavedata_factory->GetMountPoint().c_str());

    auto sharedextsavedata_factory = Common::make_unique<FileSys::ArchiveFactory_ExtSaveData>(nand_directory, true);
    if (sharedextsavedata_factory->Initialize())
        RegisterArchiveType(std::move(sharedextsavedata_factory), ArchiveIdCode::SharedExtSaveData);
    else
        LOG_ERROR(Service_FS, "Can't instantiate SharedExtSaveData archive with path %s",
            sharedextsavedata_factory->GetMountPoint().c_str());

    // Create the SaveDataCheck archive, basically a small variation of the RomFS archive
    auto savedatacheck_factory = Common::make_unique<FileSys::ArchiveFactory_SaveDataCheck>(nand_directory);
    RegisterArchiveType(std::move(savedatacheck_factory), ArchiveIdCode::SaveDataCheck);

    auto systemsavedata_factory = Common::make_unique<FileSys::ArchiveFactory_SystemSaveData>(nand_directory);
    RegisterArchiveType(std::move(systemsavedata_factory), ArchiveIdCode::SystemSaveData);
}

/// Shutdown archives
void ArchiveShutdown() {
    handle_map.clear();
    id_code_map.clear();
}

} // namespace FS
} // namespace Service
