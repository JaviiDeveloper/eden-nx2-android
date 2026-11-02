// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include "common/logging.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/nup.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_layered.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/deconstructed_rom_directory.h"
#include "core/loader/nup.h"

namespace Loader {

AppLoader_Nup::AppLoader_Nup(FileSys::VirtualFile file_)
    : AppLoader(std::move(file_)) {

    nup = std::make_unique<FileSys::NUP>(file);
    if (nup->GetStatus() != ResultStatus::Success) {
        return;
    }

    const auto* base = nup->GetBasePackage();
    if (base == nullptr) {
        return;
    }

    title_id = base->title_id;
    title_name = base->title_name;
    publisher = base->publisher;
    display_version = base->display_version;
    icon_data = base->icon_data;
    nacp_data = base->nacp_data;

    // The newest update replaces the ExeFS and is layered over the base RomFS.
    const auto updates = nup->GetUpdatePackages();
    const auto* update = updates.empty() ? nullptr : updates.back();
    if (update != nullptr && update != base) {
        if (!update->display_version.empty()) {
            display_version = update->display_version;
        }
        if (!update->nacp_data.empty()) {
            nacp_data = update->nacp_data;
        }
    }

    std::vector<FileSys::VirtualDir> romfs_layers;
    if (update != nullptr && update != base && update->romfs_dir) {
        romfs_layers.push_back(update->romfs_dir);
    }
    if (base->romfs_dir) {
        romfs_layers.push_back(base->romfs_dir);
    }
    if (!romfs_layers.empty()) {
        romfs = FileSys::CreateRomFS(
            FileSys::LayeredVfsDirectory::MakeLayeredDirectory(std::move(romfs_layers)));
    }

    const auto& exefs_dir =
        update != nullptr && update->exefs_dir ? update->exefs_dir : base->exefs_dir;
    if (exefs_dir) {
        secondary_loader =
            std::make_unique<AppLoader_DeconstructedRomDirectory>(exefs_dir, false);
    }
}

AppLoader_Nup::~AppLoader_Nup() = default;

FileType AppLoader_Nup::IdentifyType(const FileSys::VirtualFile& nup_file) {
    if (nup_file == nullptr || nup_file->GetSize() < 0x80) {
        return FileType::Error;
    }

    std::array<u8, 4> magic{};
    if (nup_file->Read(magic.data(), 4) != 4) {
        return FileType::Error;
    }

    constexpr std::array<u8, 4> NUP_MAGIC = {'N', 'U', 'P', 0x01};
    if (magic == NUP_MAGIC) {
        return FileType::NUP;
    }

    return FileType::Error;
}

AppLoader::LoadResult AppLoader_Nup::Load(Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }

    if (nup->GetStatus() != ResultStatus::Success) {
        return {nup->GetStatus(), {}};
    }

    if (!secondary_loader) {
        return {ResultStatus::ErrorNoExeFS, {}};
    }

    const auto result = secondary_loader->Load(process, system);
    if (result.first == ResultStatus::Success) {
        is_loaded = true;
    }

    return result;
}

ResultStatus AppLoader_Nup::ReadRomFS(FileSys::VirtualFile& out_file) {
    if (!romfs) {
        return ResultStatus::ErrorNoRomFS;
    }
    out_file = romfs;
    return ResultStatus::Success;
}

ResultStatus AppLoader_Nup::ReadProgramId(u64& out_program_id) {
    if (title_id == 0) {
        if (secondary_loader) {
            return secondary_loader->ReadProgramId(out_program_id);
        }
        return ResultStatus::ErrorNotImplemented;
    }
    out_program_id = title_id;
    return ResultStatus::Success;
}

ResultStatus AppLoader_Nup::ReadIcon(std::vector<u8>& buffer) {
    if (icon_data.empty()) {
        if (secondary_loader) {
            return secondary_loader->ReadIcon(buffer);
        }
        return ResultStatus::ErrorNoIcon;
    }
    buffer = icon_data;
    return ResultStatus::Success;
}

ResultStatus AppLoader_Nup::ReadTitle(std::string& title) {
    if (title_name.empty()) {
        if (secondary_loader) {
            return secondary_loader->ReadTitle(title);
        }
        return ResultStatus::ErrorNoControl;
    }
    title = title_name;
    return ResultStatus::Success;
}

ResultStatus AppLoader_Nup::ReadControlData(FileSys::NACP& nacp) {
    if (nacp_data.empty()) {
        if (secondary_loader) {
            return secondary_loader->ReadControlData(nacp);
        }
        return ResultStatus::ErrorNoControl;
    }
    if (nacp_data.size() >= sizeof(FileSys::RawNACP)) {
        FileSys::NACP nacp_obj(std::make_shared<FileSys::VectorVfsFile>(nacp_data, "control.nacp"));
        nacp = nacp_obj;
        return ResultStatus::Success;
    }
    return ResultStatus::ErrorNoControl;
}

ResultStatus AppLoader_Nup::ReadDisplayVersion(std::string& version) {
    if (!display_version.empty()) {
        version = display_version;
        return ResultStatus::Success;
    }
    if (secondary_loader) {
        return secondary_loader->ReadDisplayVersion(version);
    }
    return ResultStatus::ErrorNoControl;
}

bool AppLoader_Nup::IsRomFSUpdatable() const {
    return false;
}

ResultStatus AppLoader_Nup::ReadNSOModules(Modules& modules) {
    if (secondary_loader) {
        return secondary_loader->ReadNSOModules(modules);
    }
    return ResultStatus::ErrorNotImplemented;
}

} // namespace Loader
