// Console command-list interface IDs for device slot 73 (CreateCommandListX). The console passes an IID whose PC
// counterpart is a different GUID with the same first 32 bits, so a call that asks for it fails on the PC device
// with E_NOINTERFACE. Only measured pairs go in the table:
//   - console c3827890-4024-474c-98cf-645b51dc4874: the riid the 1.0.2.1 executable passes to slot 73 (its only GUID
//     of that form in .rdata); also in TomBox's console QueryInterface list (crates/compat/d3d12_xs/src/perfctr_xs.rs
//     line 230, commit f89e4106fe of omonkeyz/xbox-emulator, GPL-3.0);
//   - PC ID3D12GraphicsCommandList6, c3827890-e548-4cfa-96cf-5689a9370f80 (Windows SDK 10.0.26100.0 d3d12.h).
// Matching is on the whole GUID. An IID that is not in the table is forwarded unchanged, and logged once.
#pragma once

#include <windows.h>
#include <guiddef.h>
#include <d3d12.h>

#include <atomic>
#include <cstdio>

namespace rt::d3d12_xs {

struct CommandListIidPair {
    GUID console;
    GUID pc;
};

inline constexpr CommandListIidPair kCommandListIidPairs[] = {
    // ID3D12GraphicsCommandList6
    {{0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}},
     {0xc3827890, 0xe548, 0x4cfa, {0x96, 0xcf, 0x56, 0x89, 0xa9, 0x37, 0x0f, 0x80}}},
};

// True for the PC command-list IIDs a title may also pass (ID3D12CommandList, ID3D12GraphicsCommandList..7): those
// need no mapping and no log line.
inline bool is_pc_command_list_iid(const GUID& iid) {
    static const GUID kPcIids[] = {
        __uuidof(ID3D12CommandList),          __uuidof(ID3D12GraphicsCommandList),
        __uuidof(ID3D12GraphicsCommandList1), __uuidof(ID3D12GraphicsCommandList2),
        __uuidof(ID3D12GraphicsCommandList3), __uuidof(ID3D12GraphicsCommandList4),
        __uuidof(ID3D12GraphicsCommandList5), __uuidof(ID3D12GraphicsCommandList6),
        __uuidof(ID3D12GraphicsCommandList7),
    };
    for (const GUID& pc : kPcIids) {
        if (IsEqualGUID(pc, iid)) return true;
    }
    return false;
}

// The PC IID for a console command-list IID in the table; `requested` itself otherwise. `mapped` (optional) reports
// whether a substitution was made.
inline const GUID& command_list_iid_for_pc(const GUID& requested, bool* mapped = nullptr) {
    for (const CommandListIidPair& pair : kCommandListIidPairs) {
        if (IsEqualGUID(pair.console, requested)) {
            if (mapped) *mapped = true;
            return pair.pc;
        }
    }
    if (mapped) *mapped = false;
    if (is_pc_command_list_iid(requested)) return requested;  // already a PC IID: nothing to report
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        std::fprintf(stderr,
                     "[rt] d3d12_xs: command list IID %08lx-%04x-%04x is not in the console table; forwarded unchanged\n",
                     static_cast<unsigned long>(requested.Data1), static_cast<unsigned>(requested.Data2),
                     static_cast<unsigned>(requested.Data3));
    }
    return requested;
}

}  // namespace rt::d3d12_xs
