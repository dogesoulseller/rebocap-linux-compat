// Overwrites entries of a C++ vtable in place.
// Vtables are in read-only relocated data, so the pages are made writable during the write.
#pragma once

#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>

namespace rebo {
    inline bool PatchVtable(void **vtable, int first, int count, void *const *values) {
        const auto page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
        const auto start = reinterpret_cast<uintptr_t>(vtable + first) & ~(page - 1);
        const auto end = (reinterpret_cast<uintptr_t>(vtable + first + count) + page - 1) & ~(page - 1);

        if (mprotect(reinterpret_cast<void *>(start), end - start, PROT_READ | PROT_WRITE) != 0) {
            return false;
        }

        for (int i = 0; i < count; i++) {
            vtable[first + i] = values[i];
        }

        mprotect(reinterpret_cast<void *>(start), end - start, PROT_READ);

        return true;
    }
} // namespace rebo
