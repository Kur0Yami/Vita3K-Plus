#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

// Gamepad state forwarded from Android key/motion events (Emulator.java via NativeLib).
// Used by ctrl.cpp only when no SDL gamepad is open (e.g. Bluetooth pads that SDL does not map).
namespace ctrl::java_pad {
inline std::atomic<uint32_t> buttons{ 0 }; // bit n = SDL_GamepadButton n
inline std::atomic<int16_t> axes[6]{};     // SDL_GamepadAxis 0..5
inline std::atomic<bool> active{ false };

inline bool button(int id) {
    return id >= 0 && id < 32 && ((buttons.load(std::memory_order_relaxed) >> id) & 1u);
}

inline int16_t axis(int id) {
    return (id >= 0 && id < 6) ? axes[id].load(std::memory_order_relaxed) : 0;
}

inline void set_button(int id, bool down) {
    if (id < 0 || id >= 32)
        return;
    const uint32_t bit = 1u << id;
    if (down)
        buttons.fetch_or(bit, std::memory_order_relaxed);
    else
        buttons.fetch_and(~bit, std::memory_order_relaxed);
    active.store(true, std::memory_order_relaxed);
}

inline void set_axis(int id, float value) {
    if (id < 0 || id >= 6)
        return;
    int16_t raw;
    if (id >= 4) // triggers: 0..1 -> 0..32767
        raw = static_cast<int16_t>(std::clamp(value, 0.0f, 1.0f) * 32767.0f);
    else // sticks: -1..1 -> -32767..32767
        raw = static_cast<int16_t>(std::clamp(value, -1.0f, 1.0f) * 32767.0f);
    axes[id].store(raw, std::memory_order_relaxed);
    active.store(true, std::memory_order_relaxed);
}
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
