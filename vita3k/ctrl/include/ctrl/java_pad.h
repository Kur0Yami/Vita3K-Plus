#pragma once

#include <atomic>
#include <cstdint>

// State of the on-screen combo buttons (input_overlay.cpp), read by ctrl.cpp.
// Physical gamepads are NOT handled here: they go through the upstream direct pad path (ctrl_set_direct_pad).
namespace ctrl::java_pad {
inline std::atomic<uint32_t> virtual_combos{ 0 }; // bit k = on-screen combo button k pressed

inline void set_virtual_combo(int slot, bool down) {
    if (slot < 0 || slot >= 5)
        return;
    const uint32_t bit = 1u << slot;
    if (down)
        virtual_combos.fetch_or(bit, std::memory_order_relaxed);
    else
        virtual_combos.fetch_and(~bit, std::memory_order_relaxed);
}

inline bool virtual_combo(int slot) {
    return slot >= 0 && slot < 5 && ((virtual_combos.load(std::memory_order_relaxed) >> slot) & 1u);
}

inline bool any_virtual_combo() {
    return virtual_combos.load(std::memory_order_relaxed) != 0;
}

} // namespace ctrl::java_pad
