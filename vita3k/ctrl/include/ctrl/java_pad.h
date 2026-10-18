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
} // namespace ctrl::java_pad
