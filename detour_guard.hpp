/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Inline detours this addon owns, installed so that they can be taken off
// again without ever writing over code that is not ours.
//
// Detours' own detach is not chain-aware.  DetourDetach restores the
// prologue bytes it saved at attach time whatever the target holds now
// (external/Detours/src/detours.cpp, DetourDetach: it reads pbRemain and
// cbRestore from the trampoline and never looks at the target), so:
//
//   - a hooker that attached ON TOP of us (DLSS Fix, MFG Unlock, Special K,
//     OptiScaler: all detour the NGX or Streamline exports in field logs)
//     has its jmp wiped by our detach, and its own later detach restores
//     "jmp into our trampoline", which Detours has freed by then;
//   - a hooker that attached BENEATH us and detaches first wipes our jmp
//     (NR silently off), and our later detach restores ITS jmp, into code it
//     has unloaded;
//   - a module that was unloaded while hooked gets our saved bytes written
//     into unmapped memory, or into whatever DLL the loader put there since.
//
// The fix is to remember, per detour, the bytes the target held before our
// commit (`pristine`) and the 5-byte jmp our commit wrote (`ours`), and to
// read the target before touching it:
//
//   kOurs      our jmp is the head: DetourDetach restores exactly what we
//              found, which is the only case where that is correct.
//   kRestored  the pristine bytes are back (the module was unloaded and
//              reloaded at the same base, or a tool restored the image):
//              nothing of ours is reachable, nothing is written.
//   kForeign   another hooker's jump: it chained on top of us.  The target
//              is left alone; our trampoline is NEUTRALIZED instead (see
//              Neutralize), so the chain, which still leads through it,
//              runs the original function and never our code.
//   kOverwritten  code that is not a jump: a hooker beneath us detached and
//              wrote the original prologue back over our jmp.  Nothing
//              reaches our detour any more; nothing is written.
//   kUnmapped  the target cannot be read: the module is gone.
//
// Every read goes through ReadCode, which catches the access violation an
// unmapped target raises, so a sweep can run on every present.

#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <vector>

#include <detours.h>

#include "../../utils/vtable.hpp"
#include "lastgasp.hpp"

namespace renodx::addons::dlss5::detour_guard {

// x64 Detours writes `E9 rel32` (jmp to the trampoline's rbCodeIn) at the
// target and fills the rest of the relocated instructions with int3
// (detours.cpp, DetourTransactionCommitEx, DETOURS_X64 branch).
inline constexpr size_t kJmpBytes = 5;

struct Record {
  uint8_t* target = nullptr;
  uint8_t pristine[kJmpBytes] = {};
  uint8_t ours[kJmpBytes] = {};
};

enum class Head : uint8_t { kOurs, kRestored, kForeign, kOverwritten, kUnmapped };

// One detour to install: `*real` holds the resolved target on entry and the
// trampoline after a successful attach, like DetourAttach's own contract.
struct Hook {
  const char* name = nullptr;
  void** real = nullptr;
  void* detour = nullptr;
  Record* record = nullptr;
};

// Reads `size` bytes, or reports that they are not readable.  The access
// violation an unmapped page raises is the expected failure; anything else
// propagates.  Inside lastgasp's expected-fault depth, so that expected
// fault is not a [first-chance] crash-log record (memcpy runs in this
// addon's image, which the recorder watches).
inline bool ReadCode(const void* at, void* out, size_t size) noexcept {
  ++lastgasp::expected_fault_depth;
  bool read = true;
  __try {
    std::memcpy(out, at, size);
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER
                  : EXCEPTION_CONTINUE_SEARCH) {
    read = false;
  }
  --lastgasp::expected_fault_depth;
  return read;
}

// TimeDateStamp and SizeOfImage from the PE header at `module`'s base, or 0
// when no image is mapped there.  An HMODULE is only a base address: a
// module unloaded and a different one loaded at the same base answers to the
// same handle, and this is what tells them apart.
inline uint64_t ImageIdentity(HMODULE module) noexcept {
  if (module == nullptr) return 0;
  const auto* base = reinterpret_cast<const uint8_t*>(module);
  IMAGE_DOS_HEADER dos = {};
  if (!ReadCode(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
    return 0;
  }
  IMAGE_NT_HEADERS nt = {};
  if (!ReadCode(base + dos.e_lfanew, &nt, sizeof(nt))
      || nt.Signature != IMAGE_NT_SIGNATURE) {
    return 0;
  }
  return (static_cast<uint64_t>(nt.FileHeader.TimeDateStamp) << 32)
         | nt.OptionalHeader.SizeOfImage;
}

inline Head Classify(const Record& record) noexcept {
  uint8_t now[kJmpBytes] = {};
  if (record.target == nullptr || !ReadCode(record.target, now, kJmpBytes)) {
    return Head::kUnmapped;
  }
  if (std::memcmp(now, record.ours, kJmpBytes) == 0) return Head::kOurs;
  if (std::memcmp(now, record.pristine, kJmpBytes) == 0) return Head::kRestored;
  // x64 inline hooks open with a control transfer: jmp rel32 (E9: Detours,
  // MinHook), jmp rel8 (EB), push imm32; ret (68), int3 (CC: breakpoint
  // hooks), jmp [rip+disp32] (FF 25: MinHook's absolute form), or mov r64,
  // imm64 then jmp r64 (REX.W B8+r).  A release-built prologue opens with none
  // of them; misreading one that does keeps the detour, which is the
  // pre-rc11 behaviour.
  const bool jump = now[0] == 0xE9 || now[0] == 0xEB || now[0] == 0x68
                    || now[0] == 0xCC || (now[0] == 0xFF && now[1] == 0x25)
                    || ((now[0] == 0x48 || now[0] == 0x49)
                        && now[1] >= 0xB8 && now[1] <= 0xBF);
  return jump ? Head::kForeign : Head::kOverwritten;
}

// Attaches every hook whose `*real` is non-null, all in one transaction when
// they all take.  A hook whose DetourAttach fails (a prologue Detours cannot
// relocate - per function, so Create can fail while Release succeeds) is
// dropped with its `*real` reset to null and the rest are committed without
// it: the caller learns exactly which ones are live instead of a module
// being reported hooked with plain export addresses in its real pointers
// (vtable::Hook's partial abort).  Returns the number attached;
// `failed` receives the number that resolved and could not be attached.
// A transaction that cannot begin (another thread owns Detours' single
// pending transaction) attaches nothing and fails nothing: it is retried.
inline size_t AttachAll(std::span<const Hook> hooks, size_t* failed) {
  std::lock_guard<std::mutex> transaction(
      renodx::utils::vtable::TransactionMutex());
  const size_t n = hooks.size();
  std::vector<void*> resolved(n, nullptr);
  size_t remaining = 0;
  for (size_t i = 0; i < n; ++i) {
    resolved[i] = *hooks[i].real;
    if (resolved[i] != nullptr) ++remaining;
    *hooks[i].real = nullptr;
    *hooks[i].record = {};
  }
  *failed = 0;
  while (remaining != 0) {
    if (!renodx::utils::vtable::BeginTransaction()) return 0;
    DetourUpdateThread(GetCurrentThread());
    bool aborted = false;
    size_t pending = 0;
    for (size_t i = 0; i < n; ++i) {
      if (resolved[i] == nullptr) continue;
      *hooks[i].real = resolved[i];
      PVOID target = nullptr;
      if (DetourAttachEx(hooks[i].real, hooks[i].detour, nullptr, &target,
                         nullptr)
              != NO_ERROR
          || target == nullptr
          || !ReadCode(target, hooks[i].record->pristine, kJmpBytes)) {
        DetourTransactionAbort();
        for (const Hook& hook : hooks) {
          *hook.real = nullptr;
          *hook.record = {};
        }
        resolved[i] = nullptr;
        --remaining;
        ++*failed;
        aborted = true;
        break;
      }
      hooks[i].record->target = static_cast<uint8_t*>(target);
      ++pending;
    }
    if (aborted) continue;
    if (DetourTransactionCommit() != NO_ERROR) {
      DetourTransactionAbort();
      for (const Hook& hook : hooks) {
        *hook.real = nullptr;
        *hook.record = {};
      }
      return 0;
    }
    for (const Hook& hook : hooks) {
      if (*hook.real == nullptr) continue;
      ReadCode(hook.record->target, hook.record->ours, kJmpBytes);
    }
    return pending;
  }
  return 0;
}

// Makes our trampoline pass straight through to the original function.
//
// x64 Detours' jmp at the target goes to the trampoline's rbCodeIn, which is
// `FF 25 disp32`: jmp through the trampoline's pbDetour field
// (detours.cpp, detour_gen_jmp_indirect(rbCodeIn, &pbDetour)).  Pointing that
// field at the trampoline's own start - which is `*real`, the relocated
// original prologue followed by a jmp back into the function - turns every
// path that still reaches our jmp (a chain another hooker built on top of
// it) into a call of the original, with no instruction of this addon's
// image on it.  The trampoline lives in memory Detours allocated, which
// outlives this DLL.  The field is 8-byte aligned (C_ASSERT'd layout, 80
// bytes into the 96-byte trampoline), so the store is atomic for any thread
// jumping through it.
//
// Everything is derived from bytes and verified before the one write: the
// jmp we recorded, the FF 25 at its destination, and our detour's address
// in the field.  A layout that does not match writes nothing.
inline bool Neutralize(void* const* real, void* detour, const Record& record) noexcept {
  if (*real == nullptr || record.target == nullptr || record.ours[0] != 0xE9) {
    return false;
  }
  int32_t rel = 0;
  std::memcpy(&rel, record.ours + 1, sizeof(rel));
  uint8_t* const code_in = record.target + kJmpBytes + rel;
  uint8_t jump[6] = {};
  if (!ReadCode(code_in, jump, sizeof(jump)) || jump[0] != 0xFF || jump[1] != 0x25) {
    return false;
  }
  int32_t displacement = 0;
  std::memcpy(&displacement, jump + 2, sizeof(displacement));
  auto* const field = reinterpret_cast<void**>(code_in + sizeof(jump) + displacement);
  void* current = nullptr;
  if (!ReadCode(field, &current, sizeof(current))) return false;
  if (current == *real) return true;
  if (current != DetourCodeFromPointer(detour, nullptr)) return false;
  DWORD protection = 0;
  if (VirtualProtect(field, sizeof(void*), PAGE_EXECUTE_READWRITE, &protection) == FALSE) {
    return false;
  }
  InterlockedExchangePointer(field, *real);
  VirtualProtect(field, sizeof(void*), protection, &protection);
  FlushInstructionCache(GetCurrentProcess(), code_in, sizeof(jump));
  return true;
}

// What Detach did, for the caller's counters.
enum class Removal : uint8_t {
  kDetached,     // we were the head; the pristine bytes are back
  kForgotten,    // restored, overwritten, unmapped or another image:
                 // nothing of ours was reachable, nothing was written
  kNeutralized,  // a foreign head: left in place, our trampoline passes through
  kStranded,     // a foreign head whose trampoline could not be neutralized
};

// Drops a detour without touching its target: for a target that no longer
// belongs to the image the detour patched (the caller compared ImageIdentity),
// where even reading the bytes proves nothing.  Detours' trampoline is left
// allocated; nothing that remains can reach it.
inline void Forget(const Hook& hook) {
  *hook.real = nullptr;
  *hook.record = {};
}

// Takes one detour off without writing over a patch that is not ours.  The
// hook's real pointer and record are cleared in every case, so the slot is
// free to hook again.  A kOurs detach whose transaction cannot run (another
// thread owns it) falls back to neutralizing, which needs no transaction.
inline Removal Detach(const Hook& hook) {
  Removal removal = Removal::kForgotten;
  if (*hook.real != nullptr) {
    const Head head = Classify(*hook.record);
    if (head == Head::kOurs) {
      std::lock_guard<std::mutex> transaction(
          renodx::utils::vtable::TransactionMutex());
      bool detached = renodx::utils::vtable::BeginTransaction();
      if (detached) {
        DetourUpdateThread(GetCurrentThread());
        detached = DetourDetach(hook.real, hook.detour) == NO_ERROR
                   && DetourTransactionCommit() == NO_ERROR;
        if (!detached) DetourTransactionAbort();
      }
      removal = detached ? Removal::kDetached
                : Neutralize(hook.real, hook.detour, *hook.record)
                    ? Removal::kNeutralized
                    : Removal::kStranded;
    } else if (head == Head::kForeign) {
      removal = Neutralize(hook.real, hook.detour, *hook.record)
                    ? Removal::kNeutralized
                    : Removal::kStranded;
    }
  }
  Forget(hook);
  return removal;
}

}  // namespace renodx::addons::dlss5::detour_guard
