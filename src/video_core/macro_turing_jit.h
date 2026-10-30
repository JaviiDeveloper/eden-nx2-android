// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#if defined(ARCHITECTURE_x86_64)
// Eden's Xbyak configuration (its hash maps): every translation unit must see the same one.
#include "common/x64/xbyak.h"
#elif defined(ARCHITECTURE_arm64)
#include <cstring>
#include <oaknut/code_block.hpp>
#include <oaknut/oaknut.hpp>
#endif

#include "video_core/macro_turing.h"

namespace Tegra {

// x86-64 or arm64 code for the TU104/GA10x MME: every instruction of the uploaded macro memory compiled
// once (each a block that falls through to the next one, branches and loops through a jump
// table), with the semantics of TuringMacroInterpreter::Execute. Title 1 calls macros ~1.3
// million times a second; the interpreter and the macro memo's lookups were ~10% of its GPU
// thread. Emits (method writes) and register reads call back into C++.
class TuringMacroJit {
public:
    using Word = std::uint32_t;
    using Result = TuringMacroInterpreter::Result;

    struct State {
        std::array<Word, 24> regs;
        Word carry;
        Word method;
        Word increment;
        Word loop_count;
        Word loop_start;
        Word loop_end;
        Word next_pc;
        Word steps;
        Word start;
        Word consumed;
        Word params_size;
        Word fail_pc;
        std::uint8_t end_next;
        std::uint8_t stopped;
        std::uint8_t cancel_end;
        std::uint8_t pad;
        Word scratch_x;
        Word scratch_y;
        const Word* params;
        Word* dram;
        void* user;
        bool (*emit)(State*, Word method, Word value);
        Word (*read)(State*, Word address);
        const std::vector<Word>* fifo;
    };

    enum Error : Word {
        None = 0,
        FifoUnderflow,
        InvalidSource,
        InvalidDestination,
        UnsupportedExtended,
        InvalidMerge,
        InvalidLoop,
        DataRamReadOutOfBounds,
        DataRamWriteOutOfBounds,
        InvalidOutput,
        UnsupportedEmit,
        OutsideCode,
        StepLimit,
    };

    static std::string_view ErrorText(Word error) {
        constexpr std::array<std::string_view, 13> texts{
            "",
            "parameter FIFO underflow",
            "invalid source register",
            "invalid destination register",
            "unsupported extended operation",
            "invalid merge destination",
            "invalid loop",
            "data RAM read out of bounds",
            "data RAM write out of bounds",
            "invalid output selector",
            "unsupported emitted method",
            "instruction outside uploaded RAM",
            "instruction limit exceeded",
        };
        return error < texts.size() ? texts[error] : "unknown";
    }

    /// Compiles `code` (3 words per instruction) when it differs from what was compiled.
    void Prepare(std::span<const Word> code, std::uint64_t generation) {
        if (program && generation == compiled_generation) {
            return;
        }
        program = std::make_unique<Program>(code);
        compiled_generation = generation;
    }

    template <typename Read, typename Emit>
    Result Execute(Word start, std::vector<Word>& parameters, std::span<Word> data_ram, Read read,
                   Emit emit) {
        State state;
        state.regs.fill(0);
        state.carry = state.method = state.increment = 0;
        state.loop_count = state.loop_start = state.loop_end = 0;
        state.consumed = 0;
        state.fail_pc = 0;
        state.end_next = state.stopped = state.cancel_end = 0;
        state.start = start;
        state.steps = 1000000;
        state.params = parameters.data();
        state.params_size = static_cast<Word>(parameters.size());
        state.dram = data_ram.data();
        state.fifo = &parameters;
        Thunks<Read, Emit> thunks{read, emit};
        state.user = &thunks;
        state.emit = &Thunks<Read, Emit>::CallEmit;
        state.read = &Thunks<Read, Emit>::CallRead;
        const Word error = program->entry(&state);
        if (error == None) {
            return {true, state.fail_pc, state.consumed, {}};
        }
        return {false, state.fail_pc, state.consumed, ErrorText(error)};
    }

private:
    template <typename Read, typename Emit>
    struct Thunks {
        Read& read;
        Emit& emit;
        static bool CallEmit(State* state, Word method, Word value) {
            const bool ok = static_cast<Thunks*>(state->user)->emit(method, value);
            // MME_DMA_READ_FIFOED appends to the FIFO from inside the emit.
            state->params = state->fifo->data();
            state->params_size = static_cast<Word>(state->fifo->size());
            return ok;
        }
        static Word CallRead(State* state, Word address) {
            return static_cast<Thunks*>(state->user)->read(address);
        }
    };

    // Instruction layout facts both backends compile from.
    struct Analysis {
        using Decoded = TuringMacroInterpreter::Decoded;

        static constexpr std::size_t RegOffset(Word reg) {
            return offsetof(State, regs) + reg * sizeof(Word);
        }

        static bool IsFlow(Word op) {
            return op >= 23 && op <= 29;
        }

        // Where a loop can end (its back edge is checked only there) and which instructions can
        // run right after an END (only those check for the end of the macro).
        void Analyze(const std::vector<Decoded>& decoded) {
            const Word count = static_cast<Word>(decoded.size());
            loop_end.assign(count, false);
            after_end.assign(count, false);
            std::vector<Word> loop_starts_of_end(count, ~0U);
            const auto mark = [&](std::vector<bool>& set, std::int64_t pc) {
                if (pc >= 0 && pc < count) set[static_cast<std::size_t>(pc)] = true;
            };
            for (Word pc = 0; pc < count; ++pc) {
                for (Word lane = 0; lane < 2; ++lane) {
                    const Decoded& d = decoded[pc];
                    if (d.op[lane] == 23 && d.imm[lane] >= 2 && pc + d.imm[lane] - 1 < count) {
                        loop_end[pc + d.imm[lane] - 1] = true;
                    }
                }
            }
            for (Word pc = 0; pc < count; ++pc) {
                const Decoded& d = decoded[pc];
                if (!d.end) continue;
                mark(after_end, std::int64_t{pc} + 1);
                for (Word lane = 0; lane < 2; ++lane) {
                    const Word imm = d.imm[lane];
                    switch (d.op[lane]) {
                    case 23: mark(after_end, std::int64_t{pc} + imm); break;
                    case 24: mark(after_end, std::int64_t{pc} + (imm & 0x7fff)); break;
                    case 25: case 26: case 27: case 28: case 29:
                        mark(after_end, std::int64_t{pc} + (std::int32_t(imm << 19) >> 19));
                        break;
                    default: break;
                    }
                }
            }
            // An END at a loop's last instruction can be followed by the loop's second one.
            for (Word pc = 0; pc < count; ++pc) {
                for (Word lane = 0; lane < 2; ++lane) {
                    const Decoded& d = decoded[pc];
                    if (d.op[lane] == 23 && d.imm[lane] >= 2) {
                        const Word end = pc + d.imm[lane] - 1;
                        if (end < count && decoded[end].end) mark(after_end, std::int64_t{pc} + 1);
                    }
                }
            }
        }

        std::vector<bool> loop_end, after_end;
    };

#if defined(ARCHITECTURE_x86_64)
    struct Program : Xbyak::CodeGenerator, Analysis {
        explicit Program(std::span<const Word> code)
            : Xbyak::CodeGenerator(1 << 20, Xbyak::AutoGrow) {
            Generate(code);
            ready();
            entry = getCode<Word (*)(State*)>();
        }

        Word (*entry)(State*) = nullptr;

    private:
#ifdef _WIN32
        const Xbyak::Reg64 arg0 = rcx, arg1 = rdx, arg2 = r8;
#else
        const Xbyak::Reg64 arg0 = rdi, arg1 = rsi, arg2 = rdx;
#endif
        // rbx: State*; r12d/r13d: the instruction's FIFO loads; r14d/r15d: its ALU results.
        const Xbyak::Reg64 st = rbx;

        void Generate(std::span<const Word> code) {
            const Word count = static_cast<Word>(code.size() / 3);
            labels.resize(count);
            std::vector<Decoded> decoded(count);
            for (Word pc = 0; pc < count; ++pc) {
                decoded[pc] = TuringMacroInterpreter::Decode(
                    {code[pc * 3 + 2], code[pc * 3 + 1], code[pc * 3]});
            }
            Analyze(decoded);
            push(rbx); push(rbp); push(r12); push(r13); push(r14); push(r15);
            sub(rsp, 40);
            mov(st, arg0);
            mov(eax, dword[st + offsetof(State, start)]);
            cmp(eax, count);
            jae(fail_outside, T_NEAR);
            lea(rcx, ptr[rip + table]);
            jmp(ptr[rcx + rax * 8]);

            for (Word pc = 0; pc < count; ++pc) {
                L(labels[pc]);
                Instruction(pc, count, decoded[pc]);
            }

            // Dispatch on eax (the next pc). Every backward edge comes through here, so the
            // instruction limit is counted here.
            L(dispatch);
            sub(dword[st + offsetof(State, steps)], 1);
            jz(fail_steps, T_NEAR);
            cmp(eax, count);
            jae(fail_outside, T_NEAR);
            lea(rcx, ptr[rip + table]);
            jmp(ptr[rcx + rax * 8]);

            L(success);
            xor_(eax, eax);
            L(leave);
            add(rsp, 40);
            pop(r15); pop(r14); pop(r13); pop(r12); pop(rbp); pop(rbx);
            ret();
            const auto fail_with = [&](Xbyak::Label& label, Word error) {
                L(label);
                mov(eax, error);
                jmp(leave, T_NEAR);
            };
            fail_with(fail_underflow, FifoUnderflow);
            fail_with(fail_source, InvalidSource);
            fail_with(fail_destination, InvalidDestination);
            fail_with(fail_extended, UnsupportedExtended);
            fail_with(fail_merge, InvalidMerge);
            fail_with(fail_loop, InvalidLoop);
            fail_with(fail_dread, DataRamReadOutOfBounds);
            fail_with(fail_dwrite, DataRamWriteOutOfBounds);
            fail_with(fail_output, InvalidOutput);
            fail_with(fail_emit, UnsupportedEmit);
            fail_with(fail_outside, OutsideCode);
            fail_with(fail_steps, StepLimit);

            align(8);
            L(table);
            for (Word pc = 0; pc < count; ++pc) {
                putL(labels[pc]);
            }
        }

        // Loads MME register `reg` (as an operand of `lane`) into `dst`.
        void Get(const Xbyak::Reg32& dst, const Decoded& d, Word reg, Word lane) {
            if (reg < 24) {
                mov(dst, dword[st + RegOffset(reg)]);
                return;
            }
            switch (reg) {
            case 24: xor_(dst, dst); return;
            case 25: mov(dst, Word(std::int32_t(std::int16_t(d.imm[lane])))); return;
            case 26: mov(dst, Word(std::int32_t(std::int16_t(d.imm[1 - lane])))); return;
            case 27: mov(dst, (d.imm[0] << 16) | d.imm[1]); return;
            case 28: mov(dst, r12d); return;
            case 29: mov(dst, r13d); return;
            default: jmp(fail_source, T_NEAR); return;
            }
        }

        void Instruction(Word pc, Word count, const Decoded& d) {
            // FIFO loads, in lane order.
            for (Word i = 0; i < 2; ++i) {
                const Xbyak::Reg32 load = i == 0 ? r12d : r13d;
                if (!d.load[i]) {
                    xor_(load, load);
                    continue;
                }
                mov(eax, dword[st + offsetof(State, consumed)]);
                cmp(eax, dword[st + offsetof(State, params_size)]);
                jae(fail_underflow, T_NEAR);
                mov(rcx, qword[st + offsetof(State, params)]);
                mov(load, dword[rcx + rax * 4]);
                inc(eax);
                mov(dword[st + offsetof(State, consumed)], eax);
            }
            if (!d.pred_mode) {
                Body(pc, count, d, 15);
                return;
            }
            Xbyak::Label when_false;
            Get(eax, d, d.pred_reg, 0);
            test(eax, eax);
            jz(when_false, T_NEAR);
            Body(pc, count, d, d.mask_true);
            L(when_false);
            Body(pc, count, d, d.mask_false);
        }

        void Body(Word pc, Word count, const Decoded& d, Word mask) {
            // An inactive lane's result reads as 0 when an output selects it.
            for (Word lane = 0; lane < 2; ++lane) {
                if (mask & (1U << lane)) continue;
                bool selected = false;
                for (Word out = 0; out < 2; ++out) {
                    if (!(mask & (4U << out))) continue;
                    selected |= d.out_method[out] == lane + 1 || d.out_emit[out] == lane + 1;
                }
                if (selected) {
                    xor_(lane == 0 ? r14d : r15d, lane == 0 ? r14d : r15d);
                }
            }
            const bool flow = ((mask & 1) && IsFlow(d.op[0])) || ((mask & 2) && IsFlow(d.op[1]));
            if (flow) {
                mov(dword[st + offsetof(State, next_pc)], pc + 1);
                mov(byte[st + offsetof(State, cancel_end)], 0);
            }
            // Lane 1's operands are read before lane 0 commits its destination (or calls out).
            const bool lane0_active = (mask & 1) != 0;
            const bool early_operands =
                (mask & 2) && lane0_active &&
                (d.op[0] == 22 || (d.dst[0] < 24 && (d.dst[0] == d.src_a[1] || d.dst[0] == d.src_b[1])));
            if (early_operands) {
                Get(eax, d, d.src_a[1], 1);
                mov(dword[st + offsetof(State, scratch_x)], eax);
                Get(eax, d, d.src_b[1], 1);
                mov(dword[st + offsetof(State, scratch_y)], eax);
            }
            for (Word lane = 0; lane < 2; ++lane) {
                if (!(mask & (1U << lane))) {
                    continue;
                }
                if (lane == 1 && early_operands) {
                    mov(ecx, dword[st + offsetof(State, scratch_x)]);
                    mov(edx, dword[st + offsetof(State, scratch_y)]);
                } else {
                    Get(ecx, d, d.src_a[lane], lane);
                    Get(edx, d, d.src_b[lane], lane);
                }
                Lane(pc, d, lane);
                // eax holds the lane's value.
                mov(lane == 0 ? r14d : r15d, eax);
                if (d.dst[lane] < 24) {
                    mov(dword[st + RegOffset(d.dst[lane])], eax);
                } else if (d.dst[lane] != 24) {
                    jmp(fail_destination, T_NEAR);
                }
            }
            for (Word lane = 0; lane < 2; ++lane) {
                if (!(mask & (4U << lane))) {
                    continue;
                }
                if (d.out_method[lane]) {
                    Output(eax, d, d.out_method[lane]);
                    mov(ecx, eax);
                    and_(ecx, 0xfff);
                    mov(dword[st + offsetof(State, method)], ecx);
                    shr(eax, 12);
                    and_(eax, 15);
                    mov(dword[st + offsetof(State, increment)], eax);
                }
                if (d.out_emit[lane]) {
                    Output(eax, d, d.out_emit[lane]);
                    mov(arg2.cvt32(), eax);
                    mov(arg1.cvt32(), dword[st + offsetof(State, method)]);
                    mov(arg0, st);
                    call(ptr[st + offsetof(State, emit)]);
                    test(al, al);
                    jz(fail_emit, T_NEAR);
                    mov(eax, dword[st + offsetof(State, increment)]);
                    add(dword[st + offsetof(State, method)], eax);
                }
            }
            // An END in the previous instruction ends the macro after this one.
            if (after_end[pc]) {
                cmp(byte[st + offsetof(State, end_next)], 0);
                jne(success, T_NEAR);
            }
            if (flow) {
                cmp(byte[st + offsetof(State, stopped)], 0);
                jne(success, T_NEAR);
            }
            if (d.end) {
                if (flow) {
                    mov(al, byte[st + offsetof(State, cancel_end)]);
                    xor_(al, 1);
                    mov(byte[st + offsetof(State, end_next)], al);
                } else {
                    mov(byte[st + offsetof(State, end_next)], 1);
                }
            }
            if (loop_end[pc]) {
                Xbyak::Label no_loop;
                mov(eax, dword[st + offsetof(State, loop_count)]);
                test(eax, eax);
                jz(no_loop);
                cmp(dword[st + offsetof(State, loop_end)], pc);
                jne(no_loop);
                dec(dword[st + offsetof(State, loop_count)]);
                mov(eax, dword[st + offsetof(State, loop_start)]);
                inc(eax);
                jmp(dispatch, T_NEAR);
                L(no_loop);
            }
            if (flow) {
                mov(eax, dword[st + offsetof(State, next_pc)]);
                if (pc + 1 < count) {
                    cmp(eax, pc + 1);
                    jne(dispatch, T_NEAR);
                    jmp(labels[pc + 1], T_NEAR);
                } else {
                    jmp(dispatch, T_NEAR);
                }
            } else if (pc + 1 < count) {
                jmp(labels[pc + 1], T_NEAR);
            } else {
                jmp(fail_outside, T_NEAR);
            }
        }

        void Output(const Xbyak::Reg32& dst, const Decoded& d, Word selector) {
            switch (selector) {
            case 1: mov(dst, r14d); return;
            case 2: mov(dst, r15d); return;
            case 3: mov(dst, r12d); return;
            case 4: mov(dst, r13d); return;
            case 5: mov(dst, d.imm[0]); return;
            case 6: mov(dst, d.imm[1]); return;
            case 8: mov(dst, d.imm[0] >> 12); return;
            case 9: mov(dst, d.imm[1] >> 12); return;
            case 10: mov(dst, (d.imm[0] << 16) | d.imm[1]); return;
            default: jmp(fail_output, T_NEAR); return;
            }
        }

        // ecx = x, edx = y; leaves the value in eax.
        void Compare(Word op) {
            cmp(ecx, edx);
            switch (op) {
            case 17: case 25: setl(al); break;
            case 18: case 26: setb(al); break;
            case 19: case 27: setle(al); break;
            case 20: case 28: setbe(al); break;
            default: sete(al); break;
            }
            movzx(eax, al);
        }

        void Lane(Word pc, const Decoded& d, Word lane) {
            const Word imm = d.imm[lane];
            const Word op = d.op[lane];
            switch (op) {
            case 0:
                mov(eax, ecx);
                add(eax, edx);
                setc(r8b);
                movzx(r8d, r8b);
                mov(dword[st + offsetof(State, carry)], r8d);
                return;
            case 1:
                lea(eax, ptr[rcx + rdx]);
                add(eax, dword[st + offsetof(State, carry)]);
                return;
            case 2:
                mov(eax, ecx);
                sub(eax, edx);
                setc(r8b);
                movzx(r8d, r8b);
                mov(dword[st + offsetof(State, carry)], r8d);
                return;
            case 3:
                mov(eax, ecx);
                sub(eax, edx);
                sub(eax, dword[st + offsetof(State, carry)]);
                return;
            case 4:
                movsxd(rax, ecx);
                movsxd(rdx, edx);
                imul(rax, rdx);
                mov(rcx, rax);
                shr(rcx, 32);
                mov(dword[st + offsetof(State, carry)], ecx);
                return;
            case 6:
                mov(eax, ecx);
                mov(edx, edx);
                imul(rax, rdx);
                mov(rcx, rax);
                shr(rcx, 32);
                mov(dword[st + offsetof(State, carry)], ecx);
                return;
            case 5:
                mov(eax, dword[st + offsetof(State, carry)]);
                return;
            case 7:
                cmp(ecx, 0x1000);
                jne(fail_extended, T_NEAR);
                xor_(eax, eax);
                return;
            case 8: {
                Xbyak::Label zero, done;
                test(ecx, ecx);
                jz(zero);
                bsr(eax, ecx);
                xor_(eax, 31);
                jmp(done);
                L(zero);
                mov(eax, 32);
                L(done);
                return;
            }
            case 9:
                mov(eax, ecx);
                mov(ecx, edx);
                and_(ecx, 31);
                shl(eax, cl);
                return;
            case 10:
                mov(eax, ecx);
                mov(ecx, edx);
                and_(ecx, 31);
                shr(eax, cl);
                return;
            case 11:
                mov(eax, ecx);
                mov(ecx, edx);
                and_(ecx, 31);
                sar(eax, cl);
                return;
            case 12: mov(eax, ecx); and_(eax, edx); return;
            case 13: mov(eax, ecx); and_(eax, edx); not_(eax); return;
            case 14: mov(eax, ecx); or_(eax, edx); return;
            case 15: mov(eax, ecx); xor_(eax, edx); return;
            case 16: {
                const Word dest = (imm >> 10) & 63, bit_count = (imm >> 5) & 31;
                const Word source = imm & 31;
                if (dest >= 32) {
                    jmp(fail_merge, T_NEAR);
                    return;
                }
                const Word bits_mask = (1U << bit_count) - 1;
                mov(eax, ecx);
                and_(eax, ~(bits_mask << dest));
                shr(edx, source);
                and_(edx, bits_mask);
                shl(edx, dest);
                or_(eax, edx);
                return;
            }
            case 17: case 18: case 19: case 20: case 21:
                Compare(op);
                neg(eax);
                return;
            case 22:
                lea(eax, ptr[rcx + rdx]);
                and_(eax, 0xffff);
                mov(arg1.cvt32(), eax);
                mov(arg0, st);
                call(ptr[st + offsetof(State, read)]);
                return;
            case 23: {
                if (imm < 2) {
                    jmp(fail_loop, T_NEAR);
                    return;
                }
                Xbyak::Label skip, done;
                cmp(dword[st + offsetof(State, loop_count)], 0);
                jne(fail_loop, T_NEAR);
                test(ecx, ecx);
                jz(skip);
                dec(ecx);
                mov(dword[st + offsetof(State, loop_count)], ecx);
                mov(dword[st + offsetof(State, loop_start)], pc);
                mov(dword[st + offsetof(State, loop_end)], pc + imm - 1);
                jmp(done);
                L(skip);
                mov(dword[st + offsetof(State, next_pc)], pc + imm);
                L(done);
                xor_(eax, eax);
                return;
            }
            case 24:
                mov(dword[st + offsetof(State, next_pc)], pc + (imm & 0x7fff));
                xor_(eax, eax);
                return;
            case 25: case 26: case 27: case 28: case 29: {
                Xbyak::Label not_taken;
                Compare(op);
                cmp(eax, (imm & 0x8000) ? 1 : 0);
                jne(not_taken);
                const auto offset = std::int32_t(imm << 19) >> 19;
                if (offset == -4096) {
                    mov(byte[st + offsetof(State, stopped)], 1);
                } else {
                    mov(dword[st + offsetof(State, next_pc)], Word(std::int32_t(pc) + offset));
                    mov(byte[st + offsetof(State, cancel_end)], (imm & 0x4000) ? 1 : 0);
                }
                L(not_taken);
                xor_(eax, eax);
                return;
            }
            case 30:
                cmp(ecx, 0xc00);
                jae(fail_dread, T_NEAR);
                mov(rax, qword[st + offsetof(State, dram)]);
                mov(eax, dword[rax + rcx * 4]);
                return;
            case 31:
                cmp(ecx, 0xc00);
                jae(fail_dwrite, T_NEAR);
                mov(rax, qword[st + offsetof(State, dram)]);
                mov(dword[rax + rcx * 4], edx);
                xor_(eax, eax);
                return;
            default:
                xor_(eax, eax);
                return;
            }
        }

        std::vector<Xbyak::Label> labels;
        Xbyak::Label table, dispatch, success, leave;
        Xbyak::Label fail_underflow, fail_source, fail_destination, fail_extended, fail_merge,
            fail_loop, fail_dread, fail_dwrite, fail_output, fail_emit, fail_outside, fail_steps;
    };
#elif defined(ARCHITECTURE_arm64)
    // The same code as the x86-64 backend, instruction for instruction: w0 = eax, w1 = ecx,
    // w2 = edx; x19: State*; w20/w21: the instruction's FIFO loads; w22/w23: its ALU results.
    struct Program : Analysis {
        explicit Program(std::span<const Word> code) {
            std::vector<std::uint32_t> words;
            oaknut::VectorCodeGenerator c{words};
            Generate(c, code);
            const std::size_t size = ((words.size() * 4) + 0x3fff) & ~std::size_t{0x3fff};
            block = std::make_unique<oaknut::CodeBlock>(size);
            block->unprotect();
            std::memcpy(block->ptr(), words.data(), words.size() * 4);
            block->protect();
            block->invalidate(block->ptr(), words.size() * 4);
            entry = reinterpret_cast<Word (*)(State*)>(block->ptr());
        }

        Word (*entry)(State*) = nullptr;

    private:
        using Gen = oaknut::VectorCodeGenerator;
        using Cond = oaknut::Cond;

        // Conditional branches reach 1 MiB; macro code can be larger, so far targets go
        // through an unconditional branch.
        static void JumpIf(Gen& c, Cond cond, oaknut::Label& target) {
            oaknut::Label skip;
            c.B(oaknut::invert(cond), skip);
            c.B(target);
            c.l(skip);
        }
        static void JumpIfZero(Gen& c, oaknut::WReg reg, oaknut::Label& target) {
            oaknut::Label skip;
            c.CBNZ(reg, skip);
            c.B(target);
            c.l(skip);
        }
        static void JumpIfNonZero(Gen& c, oaknut::WReg reg, oaknut::Label& target) {
            oaknut::Label skip;
            c.CBZ(reg, skip);
            c.B(target);
            c.l(skip);
        }

        void Generate(Gen& c, std::span<const Word> code) {
            using namespace oaknut::util;
            const Word count = static_cast<Word>(code.size() / 3);
            labels.resize(count);
            std::vector<Decoded> decoded(count);
            for (Word pc = 0; pc < count; ++pc) {
                decoded[pc] = TuringMacroInterpreter::Decode(
                    {code[pc * 3 + 2], code[pc * 3 + 1], code[pc * 3]});
            }
            Analyze(decoded);
            c.STP(X29, X30, SP, PRE_INDEXED, -64);
            c.MOV(X29, SP);
            c.STP(X19, X20, SP, 16);
            c.STP(X21, X22, SP, 32);
            c.STR(X23, SP, 48);
            c.MOV(X19, X0);
            c.LDR(W0, X19, offsetof(State, start));
            c.B(dispatch_unchecked);

            // Dispatch on w0 (the next pc), placed ahead of the instructions so the jump table
            // is in ADR range. Every backward edge comes through here, so the instruction limit
            // is counted here.
            c.l(dispatch);
            c.LDR(W9, X19, offsetof(State, steps));
            c.SUBS(W9, W9, 1);
            c.STR(W9, X19, offsetof(State, steps));
            JumpIf(c, Cond::EQ, fail_steps);
            c.l(dispatch_unchecked);
            c.MOV(W9, count);
            c.CMP(W0, W9);
            JumpIf(c, Cond::HS, fail_outside);
            c.ADR(X9, table);
            c.ADD(X9, X9, X0, oaknut::AddSubShift::LSL, 2);
            c.BR(X9);
            c.l(table);
            for (Word pc = 0; pc < count; ++pc) {
                c.B(labels[pc]);
            }

            for (Word pc = 0; pc < count; ++pc) {
                c.l(labels[pc]);
                Instruction(c, pc, count, decoded[pc]);
            }

            c.l(success);
            c.MOV(W0, WZR);
            c.l(leave);
            c.LDR(X23, SP, 48);
            c.LDP(X21, X22, SP, 32);
            c.LDP(X19, X20, SP, 16);
            c.LDP(X29, X30, SP, POST_INDEXED, 64);
            c.RET();
            const auto fail_with = [&](oaknut::Label& label, Word error) {
                c.l(label);
                c.MOV(W0, error);
                c.B(leave);
            };
            fail_with(fail_underflow, FifoUnderflow);
            fail_with(fail_source, InvalidSource);
            fail_with(fail_destination, InvalidDestination);
            fail_with(fail_extended, UnsupportedExtended);
            fail_with(fail_merge, InvalidMerge);
            fail_with(fail_loop, InvalidLoop);
            fail_with(fail_dread, DataRamReadOutOfBounds);
            fail_with(fail_dwrite, DataRamWriteOutOfBounds);
            fail_with(fail_output, InvalidOutput);
            fail_with(fail_emit, UnsupportedEmit);
            fail_with(fail_outside, OutsideCode);
            fail_with(fail_steps, StepLimit);
        }

        // Loads MME register `reg` (as an operand of `lane`) into `dst`.
        void Get(Gen& c, oaknut::WReg dst, const Decoded& d, Word reg, Word lane) {
            using namespace oaknut::util;
            if (reg < 24) {
                c.LDR(dst, X19, RegOffset(reg));
                return;
            }
            switch (reg) {
            case 24: c.MOV(dst, WZR); return;
            case 25: c.MOV(dst, Word(std::int32_t(std::int16_t(d.imm[lane])))); return;
            case 26: c.MOV(dst, Word(std::int32_t(std::int16_t(d.imm[1 - lane])))); return;
            case 27: c.MOV(dst, (d.imm[0] << 16) | d.imm[1]); return;
            case 28: c.MOV(dst, W20); return;
            case 29: c.MOV(dst, W21); return;
            default: c.B(fail_source); return;
            }
        }

        void Instruction(Gen& c, Word pc, Word count, const Decoded& d) {
            using namespace oaknut::util;
            // FIFO loads, in lane order.
            for (Word i = 0; i < 2; ++i) {
                const oaknut::WReg load = i == 0 ? W20 : W21;
                if (!d.load[i]) {
                    c.MOV(load, WZR);
                    continue;
                }
                c.LDR(W0, X19, offsetof(State, consumed));
                c.LDR(W9, X19, offsetof(State, params_size));
                c.CMP(W0, W9);
                JumpIf(c, Cond::HS, fail_underflow);
                c.LDR(X10, X19, offsetof(State, params));
                c.LDR(load, X10, X0, oaknut::IndexExt::LSL, 2);
                c.ADD(W0, W0, 1);
                c.STR(W0, X19, offsetof(State, consumed));
            }
            if (!d.pred_mode) {
                Body(c, pc, count, d, 15);
                return;
            }
            oaknut::Label when_false;
            Get(c, W0, d, d.pred_reg, 0);
            c.CBZ(W0, when_false);
            Body(c, pc, count, d, d.mask_true);
            c.l(when_false);
            Body(c, pc, count, d, d.mask_false);
        }

        void Body(Gen& c, Word pc, Word count, const Decoded& d, Word mask) {
            using namespace oaknut::util;
            // An inactive lane's result reads as 0 when an output selects it.
            for (Word lane = 0; lane < 2; ++lane) {
                if (mask & (1U << lane)) continue;
                bool selected = false;
                for (Word out = 0; out < 2; ++out) {
                    if (!(mask & (4U << out))) continue;
                    selected |= d.out_method[out] == lane + 1 || d.out_emit[out] == lane + 1;
                }
                if (selected) {
                    c.MOV(lane == 0 ? W22 : W23, WZR);
                }
            }
            const bool flow = ((mask & 1) && IsFlow(d.op[0])) || ((mask & 2) && IsFlow(d.op[1]));
            if (flow) {
                c.MOV(W9, pc + 1);
                c.STR(W9, X19, offsetof(State, next_pc));
                c.STRB(WZR, X19, offsetof(State, cancel_end));
            }
            // Lane 1's operands are read before lane 0 commits its destination (or calls out).
            const bool lane0_active = (mask & 1) != 0;
            const bool early_operands =
                (mask & 2) && lane0_active &&
                (d.op[0] == 22 || (d.dst[0] < 24 && (d.dst[0] == d.src_a[1] || d.dst[0] == d.src_b[1])));
            if (early_operands) {
                Get(c, W0, d, d.src_a[1], 1);
                c.STR(W0, X19, offsetof(State, scratch_x));
                Get(c, W0, d, d.src_b[1], 1);
                c.STR(W0, X19, offsetof(State, scratch_y));
            }
            for (Word lane = 0; lane < 2; ++lane) {
                if (!(mask & (1U << lane))) {
                    continue;
                }
                if (lane == 1 && early_operands) {
                    c.LDR(W1, X19, offsetof(State, scratch_x));
                    c.LDR(W2, X19, offsetof(State, scratch_y));
                } else {
                    Get(c, W1, d, d.src_a[lane], lane);
                    Get(c, W2, d, d.src_b[lane], lane);
                }
                Lane(c, pc, d, lane);
                // w0 holds the lane's value.
                c.MOV(lane == 0 ? W22 : W23, W0);
                if (d.dst[lane] < 24) {
                    c.STR(W0, X19, RegOffset(d.dst[lane]));
                } else if (d.dst[lane] != 24) {
                    c.B(fail_destination);
                }
            }
            for (Word lane = 0; lane < 2; ++lane) {
                if (!(mask & (4U << lane))) {
                    continue;
                }
                if (d.out_method[lane]) {
                    Output(c, W0, d, d.out_method[lane]);
                    c.AND(W1, W0, 0xfff);
                    c.STR(W1, X19, offsetof(State, method));
                    c.UBFX(W0, W0, 12, 4);
                    c.STR(W0, X19, offsetof(State, increment));
                }
                if (d.out_emit[lane]) {
                    Output(c, W0, d, d.out_emit[lane]);
                    c.MOV(W2, W0);
                    c.LDR(W1, X19, offsetof(State, method));
                    c.MOV(X0, X19);
                    c.LDR(X9, X19, offsetof(State, emit));
                    c.BLR(X9);
                    c.AND(W0, W0, 0xff);
                    JumpIfZero(c, W0, fail_emit);
                    c.LDR(W0, X19, offsetof(State, increment));
                    c.LDR(W9, X19, offsetof(State, method));
                    c.ADD(W9, W9, W0);
                    c.STR(W9, X19, offsetof(State, method));
                }
            }
            // An END in the previous instruction ends the macro after this one.
            if (after_end[pc]) {
                c.LDRB(W9, X19, offsetof(State, end_next));
                JumpIfNonZero(c, W9, success);
            }
            if (flow) {
                c.LDRB(W9, X19, offsetof(State, stopped));
                JumpIfNonZero(c, W9, success);
            }
            if (d.end) {
                if (flow) {
                    c.LDRB(W0, X19, offsetof(State, cancel_end));
                    c.EOR(W0, W0, 1);
                    c.STRB(W0, X19, offsetof(State, end_next));
                } else {
                    c.MOV(W9, 1);
                    c.STRB(W9, X19, offsetof(State, end_next));
                }
            }
            if (loop_end[pc]) {
                oaknut::Label no_loop;
                c.LDR(W0, X19, offsetof(State, loop_count));
                c.CBZ(W0, no_loop);
                c.LDR(W9, X19, offsetof(State, loop_end));
                c.MOV(W10, pc);
                c.CMP(W9, W10);
                c.B(Cond::NE, no_loop);
                c.SUB(W0, W0, 1);
                c.STR(W0, X19, offsetof(State, loop_count));
                c.LDR(W0, X19, offsetof(State, loop_start));
                c.ADD(W0, W0, 1);
                c.B(dispatch);
                c.l(no_loop);
            }
            if (flow) {
                c.LDR(W0, X19, offsetof(State, next_pc));
                if (pc + 1 < count) {
                    c.MOV(W9, pc + 1);
                    c.CMP(W0, W9);
                    JumpIf(c, Cond::NE, dispatch);
                    c.B(labels[pc + 1]);
                } else {
                    c.B(dispatch);
                }
            } else if (pc + 1 < count) {
                c.B(labels[pc + 1]);
            } else {
                c.B(fail_outside);
            }
        }

        void Output(Gen& c, oaknut::WReg dst, const Decoded& d, Word selector) {
            using namespace oaknut::util;
            switch (selector) {
            case 1: c.MOV(dst, W22); return;
            case 2: c.MOV(dst, W23); return;
            case 3: c.MOV(dst, W20); return;
            case 4: c.MOV(dst, W21); return;
            case 5: c.MOV(dst, d.imm[0]); return;
            case 6: c.MOV(dst, d.imm[1]); return;
            case 8: c.MOV(dst, d.imm[0] >> 12); return;
            case 9: c.MOV(dst, d.imm[1] >> 12); return;
            case 10: c.MOV(dst, (d.imm[0] << 16) | d.imm[1]); return;
            default: c.B(fail_output); return;
            }
        }

        // w1 = x, w2 = y; leaves the value in w0.
        static void Compare(Gen& c, Word op) {
            using namespace oaknut::util;
            c.CMP(W1, W2);
            switch (op) {
            case 17: case 25: c.CSET(W0, Cond::LT); break;
            case 18: case 26: c.CSET(W0, Cond::LO); break;
            case 19: case 27: c.CSET(W0, Cond::LE); break;
            case 20: case 28: c.CSET(W0, Cond::LS); break;
            default: c.CSET(W0, Cond::EQ); break;
            }
        }

        void Lane(Gen& c, Word pc, const Decoded& d, Word lane) {
            using namespace oaknut::util;
            const Word imm = d.imm[lane];
            const Word op = d.op[lane];
            switch (op) {
            case 0:
                c.ADDS(W0, W1, W2);
                c.CSET(W9, Cond::CS);
                c.STR(W9, X19, offsetof(State, carry));
                return;
            case 1:
                c.ADD(W0, W1, W2);
                c.LDR(W9, X19, offsetof(State, carry));
                c.ADD(W0, W0, W9);
                return;
            case 2:
                // The carry is the borrow; arm64's C flag after SUBS is its inverse.
                c.SUBS(W0, W1, W2);
                c.CSET(W9, Cond::LO);
                c.STR(W9, X19, offsetof(State, carry));
                return;
            case 3:
                c.SUB(W0, W1, W2);
                c.LDR(W9, X19, offsetof(State, carry));
                c.SUB(W0, W0, W9);
                return;
            case 4:
            case 6:
                if (op == 4) {
                    c.SMULL(X0, W1, W2);
                } else {
                    c.UMULL(X0, W1, W2);
                }
                c.LSR(X9, X0, 32);
                c.STR(W9, X19, offsetof(State, carry));
                c.MOV(W0, W0);
                return;
            case 5:
                c.LDR(W0, X19, offsetof(State, carry));
                return;
            case 7:
                c.MOV(W9, 0x1000);
                c.CMP(W1, W9);
                JumpIf(c, Cond::NE, fail_extended);
                c.MOV(W0, WZR);
                return;
            case 8: c.CLZ(W0, W1); return;
            case 9: c.LSLV(W0, W1, W2); return;
            case 10: c.LSRV(W0, W1, W2); return;
            case 11: c.ASRV(W0, W1, W2); return;
            case 12: c.AND(W0, W1, W2); return;
            case 13: c.AND(W0, W1, W2); c.MVN(W0, W0); return;
            case 14: c.ORR(W0, W1, W2); return;
            case 15: c.EOR(W0, W1, W2); return;
            case 16: {
                const Word dest = (imm >> 10) & 63, bit_count = (imm >> 5) & 31;
                const Word source = imm & 31;
                if (dest >= 32) {
                    c.B(fail_merge);
                    return;
                }
                const Word bits_mask = (1U << bit_count) - 1;
                c.MOV(W9, ~(bits_mask << dest));
                c.AND(W0, W1, W9);
                c.LSR(W10, W2, source);
                c.MOV(W9, bits_mask);
                c.AND(W10, W10, W9);
                c.LSL(W10, W10, dest);
                c.ORR(W0, W0, W10);
                return;
            }
            case 17: case 18: case 19: case 20: case 21:
                Compare(c, op);
                c.NEG(W0, W0);
                return;
            case 22:
                c.ADD(W0, W1, W2);
                c.AND(W1, W0, 0xffff);
                c.MOV(X0, X19);
                c.LDR(X9, X19, offsetof(State, read));
                c.BLR(X9);
                return;
            case 23: {
                if (imm < 2) {
                    c.B(fail_loop);
                    return;
                }
                oaknut::Label skip, done;
                c.LDR(W9, X19, offsetof(State, loop_count));
                JumpIfNonZero(c, W9, fail_loop);
                c.CBZ(W1, skip);
                c.SUB(W1, W1, 1);
                c.STR(W1, X19, offsetof(State, loop_count));
                c.MOV(W9, pc);
                c.STR(W9, X19, offsetof(State, loop_start));
                c.MOV(W9, pc + imm - 1);
                c.STR(W9, X19, offsetof(State, loop_end));
                c.B(done);
                c.l(skip);
                c.MOV(W9, pc + imm);
                c.STR(W9, X19, offsetof(State, next_pc));
                c.l(done);
                c.MOV(W0, WZR);
                return;
            }
            case 24:
                c.MOV(W9, pc + (imm & 0x7fff));
                c.STR(W9, X19, offsetof(State, next_pc));
                c.MOV(W0, WZR);
                return;
            case 25: case 26: case 27: case 28: case 29: {
                oaknut::Label not_taken;
                Compare(c, op);
                c.CMP(W0, (imm & 0x8000) ? 1 : 0);
                c.B(Cond::NE, not_taken);
                const auto offset = std::int32_t(imm << 19) >> 19;
                if (offset == -4096) {
                    c.MOV(W9, 1);
                    c.STRB(W9, X19, offsetof(State, stopped));
                } else {
                    c.MOV(W9, Word(std::int32_t(pc) + offset));
                    c.STR(W9, X19, offsetof(State, next_pc));
                    c.MOV(W9, (imm & 0x4000) ? 1 : 0);
                    c.STRB(W9, X19, offsetof(State, cancel_end));
                }
                c.l(not_taken);
                c.MOV(W0, WZR);
                return;
            }
            case 30:
                c.MOV(W9, 0xc00);
                c.CMP(W1, W9);
                JumpIf(c, Cond::HS, fail_dread);
                c.LDR(X9, X19, offsetof(State, dram));
                c.LDR(W0, X9, X1, oaknut::IndexExt::LSL, 2);
                return;
            case 31:
                c.MOV(W9, 0xc00);
                c.CMP(W1, W9);
                JumpIf(c, Cond::HS, fail_dwrite);
                c.LDR(X9, X19, offsetof(State, dram));
                c.STR(W2, X9, X1, oaknut::IndexExt::LSL, 2);
                c.MOV(W0, WZR);
                return;
            default:
                c.MOV(W0, WZR);
                return;
            }
        }

        std::unique_ptr<oaknut::CodeBlock> block;
        std::vector<oaknut::Label> labels;
        oaknut::Label table, dispatch, dispatch_unchecked, success, leave;
        oaknut::Label fail_underflow, fail_source, fail_destination, fail_extended, fail_merge,
            fail_loop, fail_dread, fail_dwrite, fail_output, fail_emit, fail_outside, fail_steps;
    };
#endif

    std::unique_ptr<Program> program;
    std::uint64_t compiled_generation{~std::uint64_t{0}};
};

} // namespace Tegra
