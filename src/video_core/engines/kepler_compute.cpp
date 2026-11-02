// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <bitset>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "common/assert.h"
#include "common/logging.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "core/core.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/native_query_trace.h"
#if defined(ARCHITECTURE_x86_64) || defined(ARCHITECTURE_arm64)
#include "video_core/macro_turing_jit.h"
#endif
#include "video_core/rasterizer_interface.h"
#include "video_core/textures/decoders.h"

namespace Tegra::Engines {

KeplerCompute::KeplerCompute(MemoryManager& memory_manager_)
    : memory_manager{memory_manager_}
    , upload_state{memory_manager, regs.upload}
{
    // EDEN_NX2_QMD_TRACE: log the QMD scheduler's loads, dependency counts and launches.
    if (std::getenv("EDEN_NX2_QMD_TRACE")) {
        native_qmd.trace = [this](std::string_view event, u64 address, s32 value) {
            LOG_INFO(HW_GPU, "QMD trace {} {:#x} {} (engine {} as {})", event, address, value,
                     fmt::ptr(this), memory_manager.GetID());
        };
    }
    execution_mask.reset();
    execution_mask[KEPLER_COMPUTE_REG_INDEX(exec_upload)] = true;
    execution_mask[KEPLER_COMPUTE_REG_INDEX(data_upload)] = true;
    execution_mask[KEPLER_COMPUTE_REG_INDEX(launch)] = true;
    // Everything ProcessNativeMethod acts on besides storing the value; the QMD and 0x5a
    // trace range too while native draw diagnostics trace it.
    native_plain_methods.set();
    for (const u32 method : {0x45U, 0x46U, 0x47U, 0x48U, 0xaeU, 0xafU, 0xb0U, 0x107U, 0x6c3U,
                             static_cast<u32>(KEPLER_COMPUTE_REG_INDEX(exec_upload)),
                             static_cast<u32>(KEPLER_COMPUTE_REG_INDEX(data_upload))}) {
        native_plain_methods[method] = false;
    }
    if (std::getenv("EDEN_NATIVE_DRAW_DIAGNOSTICS") != nullptr) {
        native_plain_methods[0x5a] = false;
        for (u32 method = 0xc6; method <= 0x108; ++method) {
            native_plain_methods[method] = false;
        }
    }
}

KeplerCompute::~KeplerCompute() = default;

void KeplerCompute::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
    upload_state.BindRasterizer(rasterizer);
}

void KeplerCompute::ConsumeSinkImpl(Core::System& system) {
    for (auto [method, value] : method_sink) {
        if (method < Regs::NUM_REGS) {
            regs.reg_array[method] = value;
        }
    }
    method_sink.clear();
}

// SET_REPORT_SEMAPHORE_A-D (0x6c0-0x6c3): address high/low, payload, then operation in bits 1:0
// (release / acquire / report only / trap), reduction enable bit 3 with the op in bits 11:9, and
// a one-word (no timestamp) structure in bit 28. NVN2 releases queue syncs from compute this way.
void KeplerCompute::ProcessNativeReportSemaphore(u32 argument) {
    const GPUVAddr address = (GPUVAddr{native_registers[0x6c0] & 0x1ffffff} << 32) |
                             native_registers[0x6c1];
    const u32 payload = native_registers[0x6c2];
    const u32 operation = argument & 3;
    const bool one_word = (argument >> 28) & 1;
    static u32 logged{};
    if (logged++ < 32) {
        LOG_INFO(HW_GPU, "Native compute report semaphore {:#x} = {:#x}, control {:#x}", address,
                 payload, argument);
    }
    if (operation != 0 && operation != 2) {
        LOG_WARNING(HW_GPU, "Native compute report semaphore operation {} not supported", operation);
        return;
    }
    if ((argument >> 3) & 1) {
        const u32 reduction = (argument >> 9) & 7;
        rasterizer->SignalFence([this, address, payload, reduction] {
            const u32 old_value = memory_manager.Read<u32>(address);
            const u32 value = reduction == 0   ? old_value + payload
                              : reduction == 1 ? (std::min)(old_value, payload)
                              : reduction == 2 ? (std::max)(old_value, payload)
                              : reduction == 3 ? (old_value >= payload ? 0 : old_value + 1)
                              : reduction == 4 ? (old_value == 0 || old_value > payload ? payload : old_value - 1)
                              : reduction == 5 ? (old_value & payload)
                              : reduction == 6 ? (old_value | payload)
                                               : (old_value ^ payload);
            memory_manager.Write<u32>(address, value);
        });
        return;
    }
    rasterizer->Query(address, VideoCommon::QueryType::Payload,
                      one_word ? VideoCommon::QueryPropertiesFlags::IsAFence
                               : VideoCommon::QueryPropertiesFlags::HasTimeout,
                      payload, 0);
}

bool KeplerCompute::ProcessNativeMethod(Core::System& system, u32 method, u32 argument, bool is_last_call) {
    if (native_context && !front_macros) {
        if (method == 0x45 || method == 0x47) native_compute_uploaded = true;
        native_macro = native_compute_uploaded ? &native_context->compute_engine : &native_context->engine;
        native_macro_positions = native_compute_uploaded ? std::span<u32>{native_context->compute_positions} : std::span<u32>{native_context->positions};
    }
    if (method >= native_registers.size()) {
        LOG_ERROR(HW_GPU, "Native compute method out of range: {:#x}", method);
        return true;
    }
    if (method >= 0xe00) {
        if (!native_executing_macro) {
            if (method & 1) { LOG_ERROR(HW_GPU, "Native compute macro starts on argument method"); return true; }
            native_executing_macro = method;
        }
        native_parameters.push_back(argument);
        if (!is_last_call) return true;
        const u32 entry = ((*native_executing_macro - 0xe00) / 2) % native_macro_positions.size();
        native_executing_macro_logged = native_executing_macro;
        native_executing_macro.reset();
        static const u32 watch = [] {
            const char* value = std::getenv("EDEN_NX2_SEM_WATCH");
            return value ? static_cast<u32>(std::strtoull(value, nullptr, 16)) : 0U;
        }();
        const bool watched = watch != 0 && std::ranges::any_of(native_parameters,
            [](u32 parameter) { return (parameter & ~0xfU) == watch; });
        if (watched) {
            LOG_INFO(HW_GPU, "Semaphore watch: compute macro start={:#x} parameters={:08x}",
                     native_macro_positions[entry], fmt::join(native_parameters, ","));
        }
        // EDEN_NX2_MME_DUMP: write the compute macro code once per engine, and log each macro
        // call with its entry and parameters (diagnostics).
        if (static const bool mme_dump = std::getenv("EDEN_NX2_MME_DUMP") != nullptr; mme_dump) {
            if (!native_mme_dumped && native_macro_positions[entry] == 0xda) {
                native_mme_dumped = true;
                std::ofstream dump(fmt::format("native-compute-mme-{}.bin", fmt::ptr(this)),
                                   std::ios::binary);
                dump.write(reinterpret_cast<const char*>(native_macro->native_code.data()),
                           sizeof(native_macro->native_code));
            }
            LOG_INFO(HW_GPU, "MME call engine {} method {:#x} start={:#x} parameters={:08x}",
                     fmt::ptr(this), *native_executing_macro_logged, native_macro_positions[entry],
                     fmt::join(native_parameters, ","));
        }
        if (native_data_ram) native_macro->native_interpreter.active_dram = native_data_ram;
        const u32 dram0_before = (*native_macro->native_interpreter.active_dram)[0x27];
        const u32 start = native_macro_positions[entry];
        const auto emit = [&](u32 address, u32 value) {
            static u32 diagnostic_writes{};
            static const bool log_emits = std::getenv("EDEN_NX2_SEM_WATCH_EMITS") != nullptr;
            if (watched && log_emits) LOG_INFO(HW_GPU, "Semaphore watch: compute macro emit {:#x} = {:#x}", address, value);
            if (diagnostic_writes++ < 4096) LOG_DEBUG(HW_GPU, "Native compute MME emit method={:#x}, value={:#x}", address, value);
            if (address >= 0xe00) return false;
            VideoCommon::Nx2Profile::Scope method_scope{VideoCommon::Nx2Profile::Zone::Method};
            CallMethod(system, address, value, true);
            return true;
        };
#if defined(ARCHITECTURE_x86_64) || defined(ARCHITECTURE_arm64)
        // Compiled macro code, as for 3D macros (macro_turing_jit.h); EDEN_NX2_MME_JIT=0 off.
        static const bool use_jit = [] {
            const char* value = std::getenv("EDEN_NX2_MME_JIT");
            return value == nullptr || value[0] != '0';
        }();
        if (use_jit && !watched) {
            if (!native_macro->native_jit) {
                native_macro->native_jit = std::make_shared<TuringMacroJit>();
            }
            native_macro->native_jit->Prepare(native_macro->native_code,
                                              native_macro->native_code_generation);
            const auto result = native_macro->native_jit->Execute(
                start, native_parameters,
                std::span<u32>{*native_macro->native_interpreter.active_dram},
                [&](u32 address) { return address < native_registers.size() ? native_registers[address] : 0U; },
                emit);
            if (!result.success) {
                native_macro->native_execution_failed = true;
                LOG_ERROR(HW_GPU, "Native MME (JIT) failed: compute start={:#x}, consumed={}/{}, error={}",
                          start, result.consumed, native_parameters.size(), result.error);
            }
            native_parameters.clear();
            return true;
        }
#endif
        // Repeated calls replay their writes, as for 3D macros (macro_memo.h).
        static const bool use_memo = [] {
            const char* value = std::getenv("EDEN_NX2_MME_MEMO");
            return value == nullptr || value[0] != '0';
        }();
        auto& memo = native_macro->native_memo;
        if (use_memo && !watched) {
            memo.Synchronize(native_macro->native_code_generation);
            if (std::remove_reference_t<decltype(memo)>::Effects effects;
                memo.Find(start, native_parameters, *native_macro->native_interpreter.active_dram,
                          effects)) {
                auto& data_ram = *native_macro->native_interpreter.active_dram;
                for (size_t i = 0; i < effects.data_ram_writes.size(); i += 2) {
                    data_ram[effects.data_ram_writes[i]] = effects.data_ram_writes[i + 1];
                }
                for (size_t i = 0; i < effects.writes.size(); i += 2) {
                    emit(effects.writes[i], effects.writes[i + 1]);
                }
                native_parameters.clear();
                return true;
            }
            native_macro->native_writes.clear();
        }
        // The interpreter consumes native_parameters as its FIFO: keep the call's own parameters.
        native_memo_parameters.assign(native_parameters.begin(), native_parameters.end());
        const u64 code_generation = native_macro->native_code_generation;
        const auto result = native_macro->native_interpreter.Execute(native_macro->native_code,
            start, native_parameters,
            [&](u32 address) { return address < native_registers.size() ? native_registers[address] : 0U; },
            [&](u32 address, u32 value) {
                if (use_memo) {
                    native_macro->native_writes.emplace_back(address, value);
                }
                return emit(address, value);
            });
        const auto& interpreter = native_macro->native_interpreter;
        if (use_memo && !watched && result.success && !interpreter.read_registers &&
            !interpreter.reread_data_ram && code_generation == native_macro->native_code_generation) {
            memo.Store(start, native_memo_parameters, interpreter.data_ram_reads,
                       interpreter.data_ram_writes, native_macro->native_writes);
        }
        if (static const bool mme_dump = std::getenv("EDEN_NX2_MME_DUMP") != nullptr;
            mme_dump && (*native_macro->native_interpreter.active_dram)[0x27] != dram0_before) {
            LOG_INFO(HW_GPU, "MME data RAM[0x27] {:#x} -> {:#x} by engine {} macro start={:#x}",
                     dram0_before, (*native_macro->native_interpreter.active_dram)[0x27], fmt::ptr(this),
                     native_macro_positions[entry]);
        }
        if (!result.success) {
            native_macro->native_execution_failed = true;
            if (static const bool diagnostics = std::getenv("EDEN_NATIVE_DRAW_DIAGNOSTICS") != nullptr;
                diagnostics) {
                std::ofstream dump("native-compute-mme.bin", std::ios::binary);
                dump.write(reinterpret_cast<const char*>(native_macro->native_code.data()), sizeof(native_macro->native_code));
            }
            LOG_ERROR(HW_GPU, "Native MME failed: compute start={:#x}, pc={:#x}, consumed={}/{}, error={}, parameters={:08x}",
                native_macro_positions[entry], result.pc, result.consumed, native_parameters.size(), result.error,
                fmt::join(native_parameters, ","));
        }
        native_parameters.clear();
        return true;
    }
    native_registers[method] = argument;
    if (method == 0x6c3) { // SET_REPORT_SEMAPHORE_D: the compute class's semaphore release
        ProcessNativeReportSemaphore(argument);
        return true;
    }
    if (method == 0x5a || (method >= 0xc6 && method <= 0x108)) {
        VideoCommon::TraceNativeQuery(12, memory_manager.GetID(), method, 0, 0, argument, 0);
    }
    if (method == 0x107) {
        const GPUVAddr address = ((GPUVAddr{native_registers[0xc6]} << 32) |
            native_registers[0xc7]) << 8;
        const GPUVAddr program = (GPUVAddr{native_registers[0xc8 + 49] & 0x1ffff} << 32) |
            native_registers[0xc8 + 48];
        const u32 control = native_registers[0xa7];
        LOG_DEBUG(HW_GPU, "Native inline QMD: address={:#x}, program={:#x}, control={:#x}, grid={}/{}/{}",
            address, program, control, native_registers[0xc8 + 12],
            native_registers[0xc8 + 13] & 0xffff, native_registers[0xc8 + 14] & 0xffff);
        if (static const bool diagnostics = std::getenv("EDEN_NATIVE_DRAW_DIAGNOSTICS") != nullptr;
            diagnostics) {
            std::ofstream dump("native-inline-qmd.bin", std::ios::binary);
            dump.write(reinterpret_cast<const char*>(native_registers.data() + 0xc8), 256);
            std::ofstream registers("native-inline-compute-registers.bin", std::ios::binary);
            registers.write(reinterpret_cast<const char*>(native_registers.data()), sizeof(native_registers));
            if (memory_manager.IsFullyMappedRange(program, 0x10000)) {
                std::vector<u8> code(0x10000);
                memory_manager.ReadBlockUnsafe(program, code.data(), code.size());
                std::ofstream shader("native-inline-compute-shader.bin", std::ios::binary);
                shader.write(reinterpret_cast<const char*>(code.data()), code.size());
            }
        }
        if (!memory_manager.IsFullyMappedRange(address, 256)) {
            native_macro->native_execution_failed = true;
            LOG_ERROR(HW_GPU, "Unmapped native inline QMD destination");
            return true;
        }
        rasterizer->AccelerateInlineToMemory(address, 256,
            std::span<const u8>{reinterpret_cast<const u8*>(native_registers.data() + 0xc8), 256});
        if ((control & (1U << 24)) || !(native_registers[0xcc] & (1U << 12))) {
            const u32 previous = native_registers[0xad];
            native_registers[0xad] = static_cast<u32>(address >> 8);
            ProcessNativeMethod(system, 0xb0, 3, true);
            native_registers[0xad] = previous;
        }
        return true;
    }
    if (front_macros && method >= 0x45 && method <= 0x48) {
        return true; // Loaded and bound by the split GPU thread's front (FrontApply).
    }
    switch (method) {
    case 0x45: native_macro->ClearCode(argument); return true;
    case 0x46: native_macro->AddCode(0, argument); return true;
    case 0x47: native_bind_pointer = argument; return true;
    case 0x48:
        if (native_bind_pointer < native_macro_positions.size()) native_macro_positions[native_bind_pointer++] = argument;
        else LOG_ERROR(HW_GPU, "Native compute macro bind out of bounds");
        return true;
    case 0xae: case 0xaf: case 0xb0: {
        const GPUVAddr qmd_address = GPUVAddr{native_registers[0xad]} << 8;
        VideoCommon::TraceNativeQuery(9, memory_manager.GetID(), qmd_address,
            memory_manager.GpuToCpuAddress(qmd_address).value_or(0), method, argument, 0);
        if (method == 0xae || (method == 0xaf && !(argument & 2)) ||
            (method == 0xb0 && ((argument & 15) == 0 || (argument & 15) == 1))) return true;
        static const bool qmd_diagnostics = std::getenv("EDEN_NATIVE_DRAW_DIAGNOSTICS") != nullptr;
        if ((argument & 15) == 12 && qmd_diagnostics) {
            static u32 captured{};
            if (captured++ == 0 && memory_manager.IsFullyMappedRange(qmd_address, 256)) {
                std::array<u32, 64> descriptor{};
                memory_manager.ReadBlockUnsafe(qmd_address, descriptor.data(), sizeof(descriptor));
                std::ofstream dump("native-increment-qmd.bin", std::ios::binary);
                dump.write(reinterpret_cast<const char*>(descriptor.data()), sizeof(descriptor));
                LOG_WARNING(HW_GPU, "Native increment dependency descriptor: address={:#x}, words={:08x}",
                    qmd_address, fmt::join(descriptor, ","));
            }
        }
        const auto error = native_qmd.Signal(qmd_address, argument,
            [&](u64 address, NativeQmdScheduler::Descriptor& descriptor) {
                if (!memory_manager.IsFullyMappedRange(address, sizeof(descriptor))) return false;
                memory_manager.ReadBlockUnsafe(address, descriptor.data(), sizeof(descriptor));
                if (static u32 logged{}; (descriptor[53] & (1U << 23)) && logged < 8) {
                    ++logged;
                    LOG_INFO(HW_GPU, "Native QMD {:#x} releases: {:08x} | {:08x} | {:08x}", address,
                             fmt::join(std::span{descriptor}.subspan(24, 4), ","),
                             fmt::join(std::span{descriptor}.subspan(28, 4), ","),
                             fmt::join(std::span{descriptor}.subspan(52, 4), ","));
                }
                for (u32 slot = 0; slot < 2; ++slot) {
                    const u32 flags = descriptor[25 + slot * 4];
                    if (!(flags & (1U << 23))) continue;
                    const GPUVAddr destination = (GPUVAddr{flags & 0xff} << 32) | descriptor[24 + slot * 4];
                    VideoCommon::TraceNativeQuery(7 + slot, memory_manager.GetID(), destination,
                        memory_manager.GpuToCpuAddress(destination).value_or(0),
                        descriptor[4], descriptor[26 + slot * 4], flags);
                }
                return true;
            },
            [&](u64 address, u32 payload, u32 flags) {
                if (!memory_manager.IsFullyMappedRange(address, sizeof(payload))) return false;
                if (flags & (1U << 26)) {
                    rasterizer->SignalFence([this, address, payload] {
                        const u32 value = memory_manager.Read<u32>(address) + payload;
                        memory_manager.Write<u32>(address, value);
                        if (const auto device = memory_manager.GpuToCpuAddress(address)) {
                            Tegra::Nx2SemaphoreWaiters::Get().OnWrite(*device, value);
                        }
                        static const u64 watch = [] {
                            const char* text = std::getenv("EDEN_NX2_SEM_WATCH");
                            return text ? u64{std::strtoull(text, nullptr, 16)} : u64{};
                        }();
                        if (watch != 0 && (address & ~u64{0xf}) == watch) {
                            LOG_INFO(HW_GPU, "Semaphore watch: QMD release add {:#x} -> {:#x}", address, value);
                        }
                        VideoCommon::TraceNativeQuery(1, memory_manager.GetID(), address,
                            memory_manager.GpuToCpuAddress(address).value_or(0), 0, value, 1U << 26);
                    });
                } else if (flags & (1U << 19)) {
                    // NVN2 tracks QMD completion with a max release (bit 19) into a word other
                    // releases also advance: a plain write would move it backwards and leave
                    // GEQ acquires waiting forever.
                    rasterizer->SignalFence([this, address, payload] {
                        const u32 value = (std::max)(memory_manager.Read<u32>(address), payload);
                        memory_manager.Write<u32>(address, value);
                        if (const auto device = memory_manager.GpuToCpuAddress(address)) {
                            Tegra::Nx2SemaphoreWaiters::Get().OnWrite(*device, value);
                        }
                    });
                } else {
                    static const u64 watch = [] {
                        const char* text = std::getenv("EDEN_NX2_SEM_WATCH");
                        return text ? u64{std::strtoull(text, nullptr, 16)} : u64{};
                    }();
                    if (watch != 0 && (address & ~u64{0xf}) == (watch & ~u64{0xf})) {
                        LOG_INFO(HW_GPU, "Semaphore watch: QMD release {:#x} = {:#x} flags {:#x}", address, payload, flags);
                    }
                    rasterizer->Query(address, VideoCommon::QueryType::Payload,
                        VideoCommon::QueryPropertiesFlags::IsAFence, payload, 0);
                }
                return true;
            }, [&](const auto& descriptor) { return DispatchNativeCompute(descriptor); });
        if (error.empty()) {
            LOG_DEBUG(HW_GPU, "Native QMD control action={:#x}, QMD={:#x}", argument, qmd_address);
            return true;
        }
        native_macro->native_execution_failed = true;
        std::array<u32, 128> qmd{};
        if (memory_manager.IsFullyMappedRange(qmd_address, sizeof(qmd))) {
            memory_manager.ReadBlockUnsafe(qmd_address, qmd.data(), sizeof(qmd));
        }
        // Log a failing program's first few dispatches only (QMD words 48-49 hold its code
        // address); it fails the same way every frame.
        static std::unordered_map<u64, u32> failures;
        const u64 program = (u64{qmd[49]} << 32) | qmd[48];
        if (failures[program ^ std::hash<std::string>{}(std::string{error})]++ >= 2) {
            return true;
        }
        LOG_ERROR(HW_GPU, "Native QMD failed: {}", error);
        if (qmd_diagnostics && memory_manager.IsFullyMappedRange(qmd_address, sizeof(qmd))) {
            std::ofstream dump("native-qmd.bin", std::ios::binary);
            dump.write(reinterpret_cast<const char*>(qmd.data()), sizeof(qmd));
            std::ofstream macros("native-dispatch-mme.bin", std::ios::binary);
            macros.write(reinterpret_cast<const char*>(native_macro->native_code.data()), sizeof(native_macro->native_code));
            std::ofstream registers("native-dispatch-registers.bin", std::ios::binary);
            registers.write(reinterpret_cast<const char*>(native_registers.data()), sizeof(native_registers));
        }
        LOG_ERROR(HW_GPU, "Native compute dispatch unsupported: method={:#x}, value={:#x}, QMD={:#x}, words={:08x}", method, argument,
                  qmd_address, fmt::join(qmd, ","));
        return true;
    }
    default:
        // The inline upload register block is shared with the older class.
        if (method < Regs::NUM_REGS) regs.reg_array[method] = argument;
        return method != KEPLER_COMPUTE_REG_INDEX(exec_upload) && method != KEPLER_COMPUTE_REG_INDEX(data_upload);
    }
}

void KeplerCompute::CallMethod(Core::System& system, u32 method, u32 method_argument, bool is_last_call) {
    if (native_mode) {
        // Most of the compute class's register writes (QMD fields replayed by its macros for
        // each of thousands of dispatches a second) only store the value: ~4% of title 1's GPU
        // thread went through ProcessNativeMethod's checks for them.
        if (method < native_plain_methods.size() && native_plain_methods[method]) {
            native_registers[method] = method_argument;
            if (method < Regs::NUM_REGS) {
                regs.reg_array[method] = method_argument;
            }
            return;
        }
        if (ProcessNativeMethod(system, method, method_argument, is_last_call)) return;
    }
    if (method >= Regs::NUM_REGS) {
        LOG_ERROR(HW_GPU, "Compute register out of range: {:#x}", method);
        return;
    }

    regs.reg_array[method] = method_argument;

    switch (method) {
    case KEPLER_COMPUTE_REG_INDEX(exec_upload): {
        UploadInfo info{.upload_address = upload_address,
                        .exec_address = upload_state.ExecTargetAddress(),
                        .copy_size = upload_state.GetUploadSize(),
                        .was_dirty = upload_dirty};
        uploads.push_back(info);
        upload_state.ProcessExec(regs.exec_upload.linear != 0);
        break;
    }
    case KEPLER_COMPUTE_REG_INDEX(data_upload): {
        upload_address = current_dma_segment;
        upload_dirty = current_dirty;
        current_dirty = false;
        upload_state.ProcessData(method_argument, is_last_call);
        break;
    }
    case KEPLER_COMPUTE_REG_INDEX(launch): {
        const GPUVAddr launch_desc_loc = regs.launch_desc_loc.Address();

        for (auto& data : uploads) {
            const GPUVAddr offset = data.exec_address - launch_desc_loc;
            if (offset / sizeof(u32) == LAUNCH_REG_INDEX(grid_dim_x)) {
                const bool source_dirty = memory_manager.IsMemoryDirty(data.upload_address, data.copy_size);
                if (data.was_dirty || source_dirty) {
                    indirect_compute = {data.upload_address};
                }
            }
        }
        uploads.clear();
        ProcessLaunch();
        indirect_compute = std::nullopt;
        break;
    }
    default:
        break;
    }
}

void KeplerCompute::CallMultiMethod(Core::System& system, u32 method, const u32* base_start, u32 amount, u32 methods_pending) {
    if (native_mode) {
        for (u32 i = 0; i < amount; ++i) CallMethod(system, method, base_start[i], methods_pending - i <= 1);
        return;
    }
    switch (method) {
    case KEPLER_COMPUTE_REG_INDEX(data_upload):
        upload_address = current_dma_segment;
        upload_dirty = current_dirty;
        current_dirty = false;
        upload_state.ProcessData(base_start, amount);
        return;
    default:
        for (u32 i = 0; i < amount; i++) {
            CallMethod(system, method, base_start[i], methods_pending - i <= 1);
        }
        break;
    }
}

bool KeplerCompute::DispatchNativeCompute(const NativeQmdScheduler::Descriptor& qmd) {
    launch_description = {};
    launch_description.grid_dim_x.Assign(qmd[12]);
    launch_description.grid_dim_y.Assign(qmd[13] & 0xffff);
    launch_description.grid_dim_z.Assign(qmd[14] & 0xffff);
    launch_description.shared_alloc.Assign(qmd[17] & 0x3ffff);
    launch_description.block_dim_x.Assign(qmd[18] >> 16);
    launch_description.block_dim_y.Assign(qmd[19] & 0xffff);
    launch_description.block_dim_z.Assign(qmd[19] >> 16);
    launch_description.const_buffer_enable_mask.Assign(qmd[20] & 0xff);
    launch_description.local_pos_alloc.Assign(qmd[23] & 0xffffff);
    launch_description.local_crs_alloc.Assign(qmd[50] & 0xffffff);
    regs.code_loc.address_low = qmd[48];
    regs.code_loc.address_high = qmd[49] & 0x1ffff;
    for (u32 slot = 0; slot < NumConstBuffers; ++slot) {
        const u32 high = qmd[33 + slot * 2];
        if (high & 0x1ff00) {
            LOG_ERROR(HW_GPU, "Native compute constant address exceeds supported GPU width");
            return false;
        }
        auto& buffer = launch_description.const_buffer_config[slot];
        buffer.address_low = qmd[32 + slot * 2];
        buffer.address_high.Assign(high & 0xff);
        buffer.size.Assign(((high >> 19) & 0x1fff) << 4);
    }
    if (!launch_description.block_dim_x || !launch_description.block_dim_y || !launch_description.block_dim_z)
        return false;
    try {
        rasterizer->DispatchCompute();
    } catch (const Shader::Exception& error) {
        static std::unordered_set<std::string> logged;
        if (logged.insert(error.what()).second) {
            LOG_ERROR(HW_GPU, "Native compute shader rejected: {}", error.what());
        }
        if (std::string_view{error.what()}.find("matrix multiply") != std::string_view::npos) {
            DescribeNativeResources(qmd);
            // The console runs this on tensor cores; stand in with a plain upscale of its input.
            return UpscaleInsteadOfNativeProgram();
        }
        RememberDlssMotionVectors();
        return false;
    }
    return true;
}

// Logs the images a rejected program's handles (constant bank 0, bytes 0x80-0x11c) point to, so a
// substitute (e.g. an upscale blit for a tensor-core pass) can find its inputs and output.
void KeplerCompute::DescribeNativeResources(const NativeQmdScheduler::Descriptor& qmd) {
    static u32 described{};
    if (described++ >= 2) return;
    const auto& cbuf = launch_description.const_buffer_config[0];
    const GPUVAddr cbuf_address = (GPUVAddr{cbuf.address_high} << 32) | cbuf.address_low;
    LOG_INFO(HW_GPU, "Native resources: cbuf0 {:#x} size {:#x}, grid {}x{}x{}, block {}x{}x{}, tic pool {:#x}",
             cbuf_address, cbuf.size.Value(), qmd[12], qmd[13] & 0xffff, qmd[14] & 0xffff,
             launch_description.block_dim_x.Value(), launch_description.block_dim_y.Value(),
             launch_description.block_dim_z.Value(), regs.tic.Address());
    for (u32 offset = 0x80; offset < 0x120; offset += 4) {
        const u32 handle = memory_manager.Read<u32>(cbuf_address + offset);
        const u32 tic_index = handle & 0xfffff;
        if (handle == 0 || tic_index > regs.tic.limit) continue;
        const auto tic = GetTICEntry(tic_index);
        LOG_INFO(HW_GPU, "  handle @{:#x} = {:#x}: tic {} address {:#x} {}x{} format {:#x} types {}/{}/{}/{}",
                 offset, handle, tic_index, tic.Address(), tic.Width(), tic.Height(),
                 static_cast<u32>(tic.format.Value()), static_cast<u32>(tic.r_type.Value()),
                 static_cast<u32>(tic.g_type.Value()), static_cast<u32>(tic.b_type.Value()),
                 static_cast<u32>(tic.a_type.Value()));
    }
}

// The texture and surface instructions of a native program with a direct descriptor (constant
// buffer at bits 54-58, handle word offset at 40-53), so a substitute can find its images.
const std::vector<KeplerCompute::NativeImageReference>& KeplerCompute::NativeImageReferences(
    GPUVAddr program) {
    const auto [it, inserted] = native_image_references.try_emplace(program);
    if (!inserted) return it->second;
    using Shader::Ampere::Instruction;
    Shader::Ampere::ProgramCaptureState capture;
    for (u32 index = 0; index < Shader::Ampere::ProgramCaptureState::MaximumInstructions; ++index) {
        const GPUVAddr address = program + index * sizeof(Instruction);
        if (!memory_manager.IsFullyMappedRange(address, sizeof(Instruction))) break;
        Instruction insn;
        memory_manager.ReadBlockUnsafe(address, &insn, sizeof(insn));
        switch (insn.Bits(0, 12)) {
        case 0xb60: // TEX
        case 0xb63: // TLD4
        case 0xb66: // TLD
        case 0xb6c: // TXD
        case 0xb97: // SULD
        case 0xb9b: { // SUST
            const NativeImageReference ref{insn.Bits(54, 5), insn.Bits(40, 14), insn.Bits(0, 12) == 0xb9b};
            if (std::ranges::none_of(it->second, [&](const NativeImageReference& other) {
                    return other.binding == ref.binding && other.offset == ref.offset && other.store == ref.store;
                })) {
                it->second.push_back(ref);
            }
            break;
        }
        default:
            break;
        }
        if (capture.Observe(insn, index) != Shader::Ampere::ProgramCaptureState::Result::Continue) break;
    }
    return it->second;
}

std::atomic<u32> g_nx2_trace_draws_after_upscale{};

// Fire Emblem: Fortune's Weave's first DLSS pass (rejected, before the network) has the input colour
// at cbuf0+0x278, its motion vectors (R16G16 float, the input's size) at +0x290 and this frame's
// camera jitter (x, y; input pixels, Halton bases 2 and 3 less 0.5) at +0x1b0.
void KeplerCompute::RememberDlssMotionVectors() {
    const auto& cbuf = launch_description.const_buffer_config[0];
    if (cbuf.size.Value() < 0x294) return;
    const GPUVAddr cbuf_address = (GPUVAddr{cbuf.address_high} << 32) | cbuf.address_low;
    const auto image = [&](u32 offset) -> std::optional<Texture::TICEntry> {
        const u32 tic_index = memory_manager.Read<u32>(cbuf_address + offset) & 0xfffff;
        if (tic_index > regs.tic.limit) return std::nullopt;
        return GetTICEntry(tic_index);
    };
    const auto color = image(0x278), motion = image(0x290);
    if (!color || !motion || color->format != Texture::TextureFormat::B10G11R11 ||
        motion->format != Texture::TextureFormat::R16G16 || motion->Address() == 0 ||
        motion->Width() != color->Width() || motion->Height() != color->Height() ||
        !motion->IsBlockLinear())
        return;
    dlss_motion_vectors = *motion;
    dlss_motion_color = color->Address();
    dlss_jitter = {std::bit_cast<f32>(memory_manager.Read<u32>(cbuf_address + 0x1b0)),
                   std::bit_cast<f32>(memory_manager.Read<u32>(cbuf_address + 0x1b4))};
}

// Title 4's upscaler reads the tonemapped frame through the texture handle at cbuf0+0x90 and
// writes the output resolution image through the surface handle at cbuf0+0xa0 (from its
// TLD/SUST operands). DKB's reads a 960x540 B10G11R11 colour image and writes the 2880x1620
// A8B8G8R8 window image. Fire Emblem: Fortune's Weave's DLSS network ends with a pass that reads
// the input colour through cbuf0+0x328 and writes the DLSS output through cbuf0+0x348
// (B10G11R11; 720p to 1440p docked). Blit input to output with bilinear filtering when both are
// plain 2D colour images and the output is larger.
bool KeplerCompute::UpscaleInsteadOfNativeProgram() {
    const auto& cbuf = launch_description.const_buffer_config[0];
    const GPUVAddr cbuf_address = (GPUVAddr{cbuf.address_high} << 32) | cbuf.address_low;
    const auto image = [&](u32 offset) -> std::optional<Texture::TICEntry> {
        const u32 handle = memory_manager.Read<u32>(cbuf_address + offset);
        const u32 tic_index = handle & 0xfffff;
        if (handle == 0 || tic_index > regs.tic.limit) return std::nullopt;
        return GetTICEntry(tic_index);
    };
    using RTFormat = Tegra::RenderTargetFormat;
    const auto render_format = [](Texture::TextureFormat format) -> std::optional<RTFormat> {
        switch (format) {
        case Texture::TextureFormat::A2B10G10R10: return RTFormat::A2B10G10R10_UNORM;
        case Texture::TextureFormat::A8B8G8R8: return RTFormat::A8B8G8R8_UNORM;
        case Texture::TextureFormat::B10G11R11: return RTFormat::B10G11R11_FLOAT;
        case Texture::TextureFormat::R16G16B16A16: return RTFormat::R16G16B16A16_FLOAT;
        default: return std::nullopt;
        }
    };
    const auto usable = [&](u32 offset) -> std::optional<Texture::TICEntry> {
        const auto tic = image(offset);
        if (!tic || tic->Address() == 0 || !tic->IsBlockLinear() || !render_format(tic->format))
            return std::nullopt;
        return tic;
    };
    const GPUVAddr program = regs.code_loc.Address();
    const auto area = [](const Texture::TICEntry& tic) { return u64{tic.Width()} * tic.Height(); };
    // DKB's second program (0x2116182900) runs at a dynamic input resolution: cbuf0+0x18/0x1c
    // hold the rendered input extent (1396x785) and +0x28/0x2c the output extent minus one
    // (1919x1079). Only trusted when the latter matches the chosen output.
    const u32 crop_width = memory_manager.Read<u32>(cbuf_address + 0x18);
    const u32 crop_height = memory_manager.Read<u32>(cbuf_address + 0x1c);
    const u32 output_last_x = memory_manager.Read<u32>(cbuf_address + 0x28);
    const u32 output_last_y = memory_manager.Read<u32>(cbuf_address + 0x2c);
    const auto input_extent = [&](const Texture::TICEntry& tic, const Texture::TICEntry& out) {
        if (output_last_x + 1 == out.Width() && output_last_y + 1 == out.Height() && crop_width &&
            crop_height && crop_width <= tic.Width() && crop_height <= tic.Height()) {
            return std::pair{crop_width, crop_height};
        }
        return std::pair{tic.Width(), tic.Height()};
    };
    // An input fits when its (cropped) extent is smaller than the output with the same aspect
    // ratio, within 5%.
    const auto fits = [&](const Texture::TICEntry& tic, const Texture::TICEntry& out) {
        const auto [width, height] = input_extent(tic, out);
        if (width > out.Width() || height > out.Height() || u64{width} * height >= area(out))
            return false;
        const u64 cross = u64{width} * out.Height();
        const u64 expected = u64{out.Width()} * height;
        return (cross > expected ? cross - expected : expected - cross) * 20 <= expected;
    };
    std::optional<Texture::TICEntry> input, output;
    u32 input_offset{}, output_offset{};
    const char* pair_source = "program";
    // First the images the program itself samples and stores to (its direct-descriptor texture
    // and surface instructions name the constant buffer and word holding each handle).
    const auto& references = NativeImageReferences(program);
    const auto resolve = [&](const NativeImageReference& ref) -> std::optional<Texture::TICEntry> {
        if (ref.binding >= NumConstBuffers ||
            !((launch_description.const_buffer_enable_mask.Value() >> ref.binding) & 1))
            return std::nullopt;
        const auto& config = launch_description.const_buffer_config[ref.binding];
        if (ref.offset * 4U >= config.size.Value()) return std::nullopt;
        const GPUVAddr base = (GPUVAddr{config.address_high} << 32) | config.address_low;
        const u32 handle = memory_manager.Read<u32>(base + ref.offset * 4U);
        const u32 tic_index = handle & 0xfffff;
        if (handle == 0 || tic_index > regs.tic.limit) return std::nullopt;
        const auto tic = GetTICEntry(tic_index);
        using Type = Texture::TextureType;
        const Type type = tic.texture_type.Value();
        if (tic.Address() == 0 || !tic.IsBlockLinear() || !render_format(tic.format) ||
            (type != Type::Texture2D && type != Type::Texture2DNoMipmap && type != Type::Texture2DArray))
            return std::nullopt;
        return tic;
    };
    const auto packed = [](const NativeImageReference& ref) { return ref.binding << 16 | ref.offset * 4U; };
    for (const auto& ref : references) {
        if (!ref.store) continue;
        if (const auto tic = resolve(ref); tic && (!output || area(*tic) > area(*output))) {
            output = tic, output_offset = packed(ref);
        }
    }
    for (const auto& ref : references) {
        if (ref.store || !output) continue;
        const auto tic = resolve(ref);
        if (!tic || tic->Address() == output->Address() || !fits(*tic, *output)) continue;
        if (!input || area(*tic) > area(*input)) input = tic, input_offset = packed(ref);
    }
    if (!input) {
        input = usable(0x90), output = usable(0xa0);
        input_offset = 0x90, output_offset = 0xa0;
        pair_source = "cbuf0";
        if (!input || !output || output->Width() <= input->Width()) {
            // Otherwise (DKB, whose handles move between dispatches) the output is the largest image
            // and the input the largest smaller one with the same aspect ratio.
            input.reset(), output.reset();
            for (u32 offset = 0x80; offset < 0x120; offset += 4) {
                if (const auto tic = usable(offset); tic && (!output || area(*tic) > area(*output))) {
                    output = tic, output_offset = offset;
                }
            }
            for (u32 offset = 0x80; output && offset < 0x120; offset += 4) {
                const auto tic = usable(offset);
                if (!tic || tic->Address() == output->Address() || !fits(*tic, *output)) continue;
                if (!input || area(*tic) > area(*input)) input = tic, input_offset = offset;
            }
        }
    }
    bool fortunes_weave = false;
    if (!input) {
        const auto fe_input = usable(0x328), fe_output = usable(0x348);
        if (fe_input && fe_output && fe_input->format == fe_output->format &&
            fe_output->Width() > fe_input->Width()) {
            input = fe_input, output = fe_output;
            input_offset = 0x328, output_offset = 0x348;
            pair_source = "cbuf0";
            fortunes_weave = true;
        }
    }
    if (!input) {
        // Skip the dispatch (its QMD still completes): the output keeps its last contents.
        static u32 failures{};
        if (failures++ < 3) {
            LOG_WARNING(HW_GPU, "Upscale fallback: no input/output pair for program {:#x} ({} image "
                        "instructions), skipping its dispatch", program, references.size());
            const auto describe = [&](std::string_view where, u32 handle) {
                const u32 tic_index = handle & 0xfffff;
                if (handle == 0 || tic_index > regs.tic.limit) return;
                const auto tic = GetTICEntry(tic_index);
                LOG_INFO(HW_GPU, "  {} handle {:#x}: tic {} address {:#x} {}x{}x{} format {:#x} type {} "
                         "{}, layer {}", where, handle, tic_index, tic.Address(), tic.Width(), tic.Height(),
                         tic.Depth(), static_cast<u32>(tic.format.Value()),
                         static_cast<u32>(tic.texture_type.Value()),
                         tic.IsBlockLinear() ? "block linear" : "pitch", tic.BaseLayer());
            };
            for (const auto& ref : references) {
                const auto& config = launch_description.const_buffer_config[ref.binding % NumConstBuffers];
                const GPUVAddr base = (GPUVAddr{config.address_high} << 32) | config.address_low;
                const auto where = fmt::format("{} cbuf{}+{:#x}", ref.store ? "store" : "read", ref.binding,
                                               ref.offset * 4U);
                const u32 handle = ref.binding < NumConstBuffers && ref.offset * 4U < config.size.Value()
                                       ? memory_manager.Read<u32>(base + ref.offset * 4U) : 0;
                LOG_INFO(HW_GPU, "  {} = {:#x}", where, handle);
                describe(where, handle);
            }
            for (u32 offset = 0; offset < 0x120; offset += 4) {
                const u32 word = memory_manager.Read<u32>(cbuf_address + offset);
                if (word != 0) LOG_INFO(HW_GPU, "  cbuf0+{:#x} = {:#x}", offset, word);
                if (offset >= 0x80) describe(fmt::format("cbuf0+{:#x}", offset), word);
            }
        }
        return true;
    }
    if (static std::unordered_set<GPUVAddr> logged_pairs; logged_pairs.insert(program).second) {
        const auto [width, height] = input_extent(*input, *output);
        LOG_INFO(HW_GPU, "Upscale fallback: program {:#x} {} {:#x} -> {:#x}, format {:#x} -> {:#x}, "
                 "input {}x{} of {}x{}", program, pair_source, input_offset, output_offset,
                 static_cast<u32>(input->format.Value()), static_cast<u32>(output->format.Value()),
                 width, height, input->Width(), input->Height());
    }
    const auto [input_width, input_height] = input_extent(*input, *output);
    const RTFormat input_format = *render_format(input->format);
    const RTFormat output_format = *render_format(output->format);
    const auto surface = [&](const Texture::TICEntry& tic, RTFormat format) {
        Fermi2D::Surface result{};
        result.format = format;
        result.linear = Fermi2D::MemoryLayout::BlockLinear;
        result.block_width.Assign(tic.block_width);
        result.block_height.Assign(tic.block_height);
        result.block_depth.Assign(tic.block_depth);
        result.depth = 1;
        result.width = tic.Width();
        result.height = tic.Height();
        result.addr_upper = static_cast<u32>(tic.Address() >> 32);
        result.addr_lower = static_cast<u32>(tic.Address());
        return result;
    };
    const Fermi2D::Config config{
        .operation = Fermi2D::Operation::SrcCopy,
        .filter = Fermi2D::Filter::Bilinear,
        .must_accelerate = true,
        .dst_x0 = 0,
        .dst_y0 = 0,
        .dst_x1 = static_cast<s32>(output->Width()),
        .dst_y1 = static_cast<s32>(output->Height()),
        .src_x0 = 0,
        .src_y0 = 0,
        .src_x1 = static_cast<s32>(input_width),
        .src_y1 = static_cast<s32>(input_height),
    };
    // Debug: does the input hold anything? Download it once and count non-zero words.
    static const u32 sample_at = [] {
        const char* value = std::getenv("EDEN_NX2_SAMPLE_AT");
        return value ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 20U;
    }();
    if (static u32 sampled{}; sampled++ == sample_at) {
        const auto sample = [&](const Texture::TICEntry& tic) {
            const u64 size = u64{tic.Width()} * tic.Height() * 4;
            if (const auto device = memory_manager.GpuToCpuAddress(tic.Address())) {
                rasterizer->FlushRegion(*device, size, VideoCommon::CacheType::All);
            }
            u32 nonzero{}, samples{};
            u64 sum{};
            for (u64 offset = 0; offset < size; offset += 4096 + 4) {
                const u32 word = memory_manager.Read<u32>(tic.Address() + offset);
                ++samples;
                nonzero += word != 0;
                sum += (word & 0x3ff) + ((word >> 10) & 0x3ff) + ((word >> 20) & 0x3ff);
            }
            LOG_INFO(HW_GPU, "Upscale fallback image {:#x}: {}/{} sampled words non-zero, mean channel {}",
                     tic.Address(), nonzero, samples, samples ? sum / (samples * 3) : 0);
        };
        sample(*input);
        sample(*output);
        // Every 2560x1440 target the in-game passes draw to (from EDEN_NX2_RT_TRACE).
        for (const GPUVAddr address : {0x8116a03000ULL, 0x8117ae5000ULL, 0x8114b62000ULL,
                                       0x8115a63000ULL, 0x8117904000ULL, 0x8114a02000ULL}) {
            Texture::TICEntry fake = *input;
            const u64 size = 2560ULL * 1440 * 4;
            if (const auto device = memory_manager.GpuToCpuAddress(address)) {
                rasterizer->FlushRegion(*device, size, VideoCommon::CacheType::All);
            }
            u32 nonzero{}, samples{}, distinct{};
            u32 last{};
            for (u64 offset = 0; offset < size; offset += 4096 + 4) {
                const u32 word = memory_manager.Read<u32>(address + offset);
                ++samples;
                nonzero += word != 0;
                distinct += word != last;
                last = word;
            }
            LOG_INFO(HW_GPU, "Upscale fallback target {:#x}: {}/{} non-zero, {} changes", address, nonzero,
                     samples, distinct);
            (void)fake;
        }
    }
    auto source = surface(*input, input_format);
    // EDEN_NX2_UPSCALE_SOURCE=<gpu address>: blit from another same-sized image (debug aid).
    if (static const u64 override_source = [] {
            const char* value = std::getenv("EDEN_NX2_UPSCALE_SOURCE");
            return value ? std::strtoull(value, nullptr, 16) : 0ULL;
        }();
        override_source != 0) {
        source.addr_upper = static_cast<u32>(override_source >> 32);
        source.addr_lower = static_cast<u32>(override_source);
        if (const char* value = std::getenv("EDEN_NX2_UPSCALE_SOURCE_FORMAT")) {
            source.format = static_cast<RTFormat>(std::strtoul(value, nullptr, 16));
        }
    }
    // Fortune's Weave: a temporal upscale along its motion vectors that undoes this frame's camera
    // jitter (both from the frame's first DLSS pass) and accumulates the frames as DLSS would.
    // EDEN_NX2_TEMPORAL_UPSCALE=0 blits instead.
    const auto upscale_temporally = [&] {
        static const bool enabled = [] {
            const char* value = std::getenv("EDEN_NX2_TEMPORAL_UPSCALE");
            return value == nullptr || std::string_view{value} != "0";
        }();
        if (!fortunes_weave || !enabled || !dlss_motion_vectors ||
            dlss_motion_color != input->Address())
            return false;
        const Fermi2D::Surface motion = surface(*dlss_motion_vectors, RTFormat::R16G16_FLOAT);
        return rasterizer->AccelerateTemporalUpscale(source, surface(*output, output_format), motion, dlss_jitter,
                                                     {1.0f, 1.0f});
    };
    const bool done = upscale_temporally() ||
                      rasterizer->AccelerateSurfaceCopy(source, surface(*output, output_format), config);
    if (static u32 checked{}; checked++ == sample_at) {
        const u64 size = u64{output->Width()} * output->Height() * 4;
        if (const auto device = memory_manager.GpuToCpuAddress(output->Address())) {
            rasterizer->FlushRegion(*device, size, VideoCommon::CacheType::All);
        }
        u32 nonzero{}, samples{}, changes{}, last{};
        for (u64 offset = 0; offset < size; offset += 4096 + 4) {
            const u32 word = memory_manager.Read<u32>(output->Address() + offset);
            ++samples;
            nonzero += word != 0;
            changes += word != last;
            last = word;
        }
        LOG_INFO(HW_GPU, "Upscale fallback output after blit: {}/{} non-zero, {} changes", nonzero, samples,
                 changes);
    }
    // Debug: trace the draws that follow one upscale (EDEN_NX2_RT_TRACE_AFTER_UPSCALE).
    static const u32 trace_at = [] {
        const char* value = std::getenv("EDEN_NX2_TRACE_AFTER_UPSCALE");
        return value ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 30U;
    }();
    if (static u32 count{}; ++count == trace_at) g_nx2_trace_draws_after_upscale = 40;
    static u32 logged{};
    if (logged++ < 4) {
        LOG_INFO(HW_GPU, "Upscale fallback {:#x} {}x{} block {}/{}/{} -> {:#x} {}x{} block {}/{}/{} levels {}: {}",
                 input->Address(), input->Width(), input->Height(), input->block_width.Value(),
                 input->block_height.Value(), input->block_depth.Value(), output->Address(),
                 output->Width(), output->Height(), output->block_width.Value(),
                 output->block_height.Value(), output->block_depth.Value(), output->max_mip_level.Value(),
                 done ? "blitted" : "not accelerated");
    }
    return done;
}

void KeplerCompute::ProcessLaunch() {
    const GPUVAddr launch_desc_loc = regs.launch_desc_loc.Address();
    memory_manager.ReadBlockUnsafe(launch_desc_loc, &launch_description,
                                   LaunchParams::NUM_LAUNCH_PARAMETERS * sizeof(u32));
    rasterizer->DispatchCompute();
}

Texture::TICEntry KeplerCompute::GetTICEntry(u32 tic_index) const {
    const GPUVAddr tic_address_gpu{regs.tic.Address() + tic_index * sizeof(Texture::TICEntry)};

    Texture::TICEntry tic_entry;
    memory_manager.ReadBlockUnsafe(tic_address_gpu, &tic_entry, sizeof(Texture::TICEntry));
    return tic_entry;
}

Texture::TSCEntry KeplerCompute::GetTSCEntry(u32 tsc_index) const {
    const GPUVAddr tsc_address_gpu{regs.tsc.Address() + tsc_index * sizeof(Texture::TSCEntry)};

    Texture::TSCEntry tsc_entry;
    memory_manager.ReadBlockUnsafe(tsc_address_gpu, &tsc_entry, sizeof(Texture::TSCEntry));
    return tsc_entry;
}

// Split GPU thread with macros on its front half (gpu_split.h, EDEN_NX2_SPLIT=2): the front
// runs compute macros against its own copy of the native registers (which hold the last value
// written to each, on the back too) and loads and binds the compute macro code itself.
struct KeplerCompute::FrontState {
    std::array<u32, 0x1000> registers{};
    std::optional<u32> executing_macro;
    std::vector<u32> parameters;
    bool compute_uploaded{};
    u32 bind_pointer{};
    MacroEngine* macro{}; ///< Without a native context: the one macro engine
};

KeplerCompute::FrontState& KeplerCompute::Front(VideoCommon::GpuSplit& split) {
    if (!front) [[unlikely]] {
        // Copy the back's state and take the macros over while it is idle.
        split.Drain();
        front = std::make_unique<FrontState>();
        front->registers = native_registers;
        front->executing_macro = native_executing_macro;
        front->parameters = std::move(native_parameters);
        native_parameters.clear();
        native_executing_macro.reset();
        front->compute_uploaded = native_compute_uploaded;
        front->bind_pointer = native_bind_pointer;
        front->macro = native_macro;
        front_macros = true;
    }
    return *front;
}

void KeplerCompute::FrontApply(FrontState& fs, u32 method, u32 argument) {
    // As ProcessNativeMethod does with the macro code and bindings.
    if (method == 0x45 || method == 0x47) {
        fs.compute_uploaded = true;
    }
    fs.registers[method] = argument;
    switch (method) {
    case 0x45:
        FrontMacroEngine(fs).ClearCode(argument);
        break;
    case 0x46:
        FrontMacroEngine(fs).AddCode(0, argument);
        break;
    case 0x47:
        fs.bind_pointer = argument;
        break;
    case 0x48: {
        const auto positions = FrontMacroPositions(fs);
        if (fs.bind_pointer < positions.size()) {
            positions[fs.bind_pointer++] = argument;
        } else {
            LOG_ERROR(HW_GPU, "Native compute macro bind out of bounds");
        }
        break;
    }
    default:
        break;
    }
}

MacroEngine& KeplerCompute::FrontMacroEngine(FrontState& fs) {
    if (native_context) {
        return fs.compute_uploaded ? native_context->compute_engine : native_context->engine;
    }
    return *fs.macro;
}

std::span<u32> KeplerCompute::FrontMacroPositions(FrontState& fs) {
    if (native_context) {
        return fs.compute_uploaded ? std::span<u32>{native_context->compute_positions}
                                      : std::span<u32>{native_context->positions};
    }
    return native_macro_positions;
}

void KeplerCompute::FrontCallMethod(Core::System& system, VideoCommon::GpuSplit& split,
                                    u32 method, u32 argument, bool is_last_call,
                                    const GPUVAddr* segment) {
    auto& fs = Front(split);
    if (method >= fs.registers.size()) {
        split.Method(this, method, argument, is_last_call, segment);
        return;
    }
    if (method >= 0xe00) {
        if (!fs.executing_macro) {
            if (method & 1) {
                LOG_ERROR(HW_GPU, "Native compute macro starts on argument method");
                return;
            }
            fs.executing_macro = method;
        }
        fs.parameters.push_back(argument);
        if (is_last_call) {
            FrontRunMacro(system, split, fs);
        }
        return;
    }
    FrontApply(fs, method, argument);
    split.Method(this, method, argument, is_last_call, segment);
}

void KeplerCompute::FrontCallMultiMethod(Core::System& system, VideoCommon::GpuSplit& split,
                                         u32 method, const u32* base_start, u32 amount,
                                         u32 methods_pending, const GPUVAddr* segment) {
    auto& fs = Front(split);
    if (method >= 0xe00 && method < fs.registers.size()) {
        for (u32 i = 0; i < amount; ++i) {
            FrontCallMethod(system, split, method, base_start[i], methods_pending - i <= 1, segment);
        }
        return;
    }
    if (method < fs.registers.size()) {
        for (u32 i = 0; i < amount; ++i) {
            FrontApply(fs, method, base_start[i]);
        }
    }
    split.MultiMethod(this, method, base_start, amount, methods_pending, segment);
}

void KeplerCompute::FrontRunMacro(Core::System& system, VideoCommon::GpuSplit& split,
                                  FrontState& fs) {
#if defined(ARCHITECTURE_x86_64) || defined(ARCHITECTURE_arm64)
    const auto positions = FrontMacroPositions(fs);
    const u32 start = positions[((*fs.executing_macro - 0xe00) / 2) % positions.size()];
    fs.executing_macro.reset();
    auto& macro = FrontMacroEngine(fs);
    if (native_data_ram) {
        macro.native_interpreter.active_dram = native_data_ram;
    }
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::Macro};
    if (!macro.native_jit) {
        macro.native_jit = std::make_shared<TuringMacroJit>();
    }
    macro.native_jit->Prepare(macro.native_code, macro.native_code_generation);
    const auto result = macro.native_jit->Execute(
        start, fs.parameters, std::span<u32>{*macro.native_interpreter.active_dram},
        [&fs](u32 address) {
            return address < fs.registers.size() ? fs.registers[address] : 0U;
        },
        [&](u32 address, u32 value) {
            if (address >= 0xe00) {
                return false;
            }
            FrontApply(fs, address, value);
            split.MacroWrite(this, address, value);
            return true;
        });
    if (!result.success) {
        macro.native_execution_failed = true;
        LOG_ERROR(HW_GPU, "Native MME (JIT, split front) failed: compute start={:#x}, consumed={}, error={}",
                  start, result.consumed, result.error);
    }
#else
    fs.executing_macro.reset();
#endif
    fs.parameters.clear();
}

} // namespace Tegra::Engines
