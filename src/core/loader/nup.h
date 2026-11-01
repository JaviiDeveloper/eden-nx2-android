// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include "common/common_types.h"
#include "core/loader/loader.h"

namespace FileSys {
class NUP;
class NACP;
} // namespace FileSys

namespace Loader {

class AppLoader_Nup final : public AppLoader {
public:
    explicit AppLoader_Nup(FileSys::VirtualFile file);
    ~AppLoader_Nup() override;

    static FileType IdentifyType(const FileSys::VirtualFile& nup_file);

    FileType GetFileType() const override {
        return IdentifyType(file);
    }

    LoadResult Load(Kernel::KProcess& process, Core::System& system) override;

    ResultStatus ReadRomFS(FileSys::VirtualFile& out_file) override;
    ResultStatus ReadProgramId(u64& out_program_id) override;
    ResultStatus ReadIcon(std::vector<u8>& buffer) override;
    ResultStatus ReadTitle(std::string& title) override;
    ResultStatus ReadControlData(FileSys::NACP& nacp) override;
    ResultStatus ReadDisplayVersion(std::string& version) override;
    bool IsRomFSUpdatable() const override;

    ResultStatus ReadNSOModules(Modules& modules) override;

private:
    std::unique_ptr<FileSys::NUP> nup;
    std::unique_ptr<AppLoader> secondary_loader;
    FileSys::VirtualFile romfs;
    std::vector<u8> icon_data;
    std::string title_name;
    std::string publisher;
    std::string display_version;
    std::vector<u8> nacp_data;
    u64 title_id{};
};

} // namespace Loader
