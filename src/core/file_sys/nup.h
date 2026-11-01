// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>
#include "common/common_types.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/loader/loader.h"

namespace FileSys {

enum class NupPackageKind : u8 {
    Base = 0,
    Update = 1,
    Addon = 2,
    AddonUpdate = 3,
    System = 4,
};

struct NupPackageInfo {
    u64 title_id{};
    u32 version{};
    NupPackageKind kind{NupPackageKind::Base};
    bool standalone_update{};
    VirtualDir root_dir;
    VirtualDir exefs_dir;
    VirtualDir romfs_dir;
    std::vector<u8> icon_data;
    std::string title_name;
    std::string publisher;
    std::string display_version;
    std::vector<u8> npdm_data;
    std::vector<u8> nacp_data;
};

class NUP {
public:
    explicit NUP(VirtualFile file);
    ~NUP();

    Loader::ResultStatus GetStatus() const;

    const std::vector<NupPackageInfo>& GetPackages() const;
    const NupPackageInfo* GetBasePackage() const;
    std::vector<const NupPackageInfo*> GetUpdatePackages() const;
    std::vector<const NupPackageInfo*> GetAddonPackages() const;

private:
    bool Parse();
    void ParseMetaTLVs(NupPackageInfo& pkg, u64 meta_offset, u32 meta_size);
    VirtualDir BuildDirectoryTree(u32 dir_index);
    VirtualFile MakeFileView(u64 extent_offset, u64 extent_size, u64 uncompressed_size,
                             u8 codec, const std::string& name);

    VirtualFile nup_file;
    Loader::ResultStatus status{Loader::ResultStatus::ErrorNotInitialized};
    std::vector<NupPackageInfo> packages;

    // Parsed TOC data
    std::vector<u8> toc_data;
    u64 toc_offset{};
    u64 dir_count{};
    u64 file_count{};
    u64 string_pool_off{};
    u64 string_pool_size{};
    u32 package_count{};

    // Offsets within TOC for each section
    u64 packages_off{};
    u64 dirs_off{};
    u64 files_off{};
    u64 children_off{};

    std::string ReadString(u32 offset, u32 length) const;
};

} // namespace FileSys
