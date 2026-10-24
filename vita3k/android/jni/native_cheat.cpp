// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include "android_state.h"
#include "cheat_core.h"
#include "cheat_psv.h"

#include <app/session_controller.h>
#include <emuenv/state.h>
#include <io/state.h>
#include <util/fs.h>

#include <jni.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

cheat::Scanner &scanner() {
    static auto *instance = new cheat::Scanner(); // intentionally leaked: the freezer thread outlives statics
    return *instance;
}

cheat::CheatList &cheats() {
    static auto *instance = new cheat::CheatList();
    return *instance;
}

std::string list_file(const EmuEnvState &emuenv) {
    return (emuenv.config_path / "cheats" / (emuenv.io.app_path + ".txt")).string();
}

std::string psv_file(const EmuEnvState &emuenv) {
    return (emuenv.config_path / "cheats" / (emuenv.io.app_path + ".psv")).string();
}

// Loads this game's list if it was not loaded yet (the freezer thread may not have ticked yet).
void ensure_loaded(EmuEnvState &emuenv) {
    cheats().ensure_loaded(emuenv.io.app_path, list_file(emuenv), psv_file(emuenv));
}

// Every change to the list is stored right away.
void autosave(EmuEnvState &emuenv) {
    const auto file = list_file(emuenv);
    boost::system::error_code ec;
    fs::create_directories(fs::path(file).parent_path(), ec);
    cheats().save(file);
}

// JNI's NewStringUTF needs valid (modified) UTF-8; names from .psv files may be ANSI. Also keeps names on one line.
std::string clean_text(const std::string &in) {
    std::string out;
    for (size_t i = 0; i < in.size();) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        size_t len = c < 0x80 ? 1 : (c >= 0xC2 && c < 0xE0) ? 2 : (c >= 0xE0 && c < 0xF0) ? 3 : 0;
        bool ok = len > 0 && i + len <= in.size();
        for (size_t k = 1; ok && k < len; k++)
            ok = (static_cast<unsigned char>(in[i + k]) & 0xC0) == 0x80;
        if (len == 4 || !ok) { // 4-byte sequences are not valid modified UTF-8 either
            out += '?';
            ++i;
            continue;
        }
        if (len == 1 && (c == '\t' || c == '\n' || c == '\r' || c < 0x20))
            out += ' ';
        else
            out.append(in, i, len);
        i += len;
    }
    return out;
}

// The live environment, only while a game session exists (pause menu included).
EmuEnvState *active_env() {
    auto *emuenv = get_emuenv();
    auto *controller = get_app_session_controller();
    return (emuenv && controller && controller->has_active_session()) ? emuenv : nullptr;
}

void freezer_loop() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto *emuenv = get_emuenv();
        auto *controller = get_app_session_controller();
        if (!emuenv || !controller || !controller->has_active_session()) {
            cheats().reset_app();
            continue;
        }
        if (!controller->is_running())
            continue;
        cheats().tick(emuenv->mem, emuenv->io.app_path, list_file(*emuenv), psv_file(*emuenv));
    }
}

struct FreezerStarter {
    FreezerStarter() {
        std::thread(freezer_loop).detach();
    }
};
const FreezerStarter g_freezer_starter;

constexpr jlong ERR_NO_GAME = -3;

} // namespace

namespace macro {
void start_record();
void stop_record();
void start_play(bool loop);
void stop_play();
int get_mode();
size_t length();
} // namespace macro

extern "C" {

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_clear(JNIEnv *, jclass) {
    scanner().clear();
}

JNIEXPORT jlong JNICALL
Java_org_vita3k_emulator_CheatNative_search(JNIEnv *, jclass, jint raw, jint type, jfloat tolerance) {
    auto *emuenv = active_env();
    if (!emuenv)
        return ERR_NO_GAME;
    return scanner().search(emuenv->mem, raw, static_cast<cheat::ValueType>(type), tolerance);
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_resetRandom(JNIEnv *, jclass) {
    cheat::psv::reset_random();
}

JNIEXPORT jlong JNICALL
Java_org_vita3k_emulator_CheatNative_searchBytes(JNIEnv *env, jclass, jbyteArray pattern) {
    auto *emuenv = active_env();
    if (!emuenv)
        return ERR_NO_GAME;
    const jsize len = env->GetArrayLength(pattern);
    std::vector<uint8_t> bytes(static_cast<size_t>(len));
    if (len > 0)
        env->GetByteArrayRegion(pattern, 0, len, reinterpret_cast<jbyte *>(bytes.data()));
    return scanner().search_bytes(emuenv->mem, bytes);
}

JNIEXPORT jlong JNICALL
Java_org_vita3k_emulator_CheatNative_snapshot(JNIEnv *, jclass) {
    auto *emuenv = active_env();
    if (!emuenv)
        return ERR_NO_GAME;
    return scanner().snapshot(emuenv->mem);
}

JNIEXPORT jlong JNICALL
Java_org_vita3k_emulator_CheatNative_compare(JNIEnv *, jclass, jint mode, jint type) {
    auto *emuenv = active_env();
    if (!emuenv)
        return ERR_NO_GAME;
    return scanner().compare_snapshot(emuenv->mem, static_cast<cheat::Compare>(mode), static_cast<cheat::ValueType>(type));
}

// mode < 0: exact match against `raw`; otherwise compare against each candidate's previous value.
JNIEXPORT jlong JNICALL
Java_org_vita3k_emulator_CheatNative_refine(JNIEnv *, jclass, jint mode, jint raw, jint type, jfloat tolerance) {
    auto *emuenv = active_env();
    if (!emuenv)
        return ERR_NO_GAME;
    std::optional<cheat::Compare> cmp;
    if (mode >= 0)
        cmp = static_cast<cheat::Compare>(mode);
    return scanner().refine(emuenv->mem, cmp, raw, static_cast<cheat::ValueType>(type), tolerance);
}

JNIEXPORT jlong JNICALL
Java_org_vita3k_emulator_CheatNative_count(JNIEnv *, jclass) {
    return static_cast<jlong>(scanner().count());
}

// Interleaved [address, value, address, value, ...]
JNIEXPORT jlongArray JNICALL
Java_org_vita3k_emulator_CheatNative_results(JNIEnv *env, jclass, jint offset, jint max) {
    const auto matches = scanner().results(static_cast<size_t>(std::max(0, static_cast<int>(offset))), static_cast<size_t>(std::max(0, static_cast<int>(max))));
    std::vector<jlong> flat;
    flat.reserve(matches.size() * 2);
    for (const auto &m : matches) {
        flat.push_back(static_cast<jlong>(m.address));
        flat.push_back(static_cast<jlong>(m.value));
    }
    jlongArray array = env->NewLongArray(static_cast<jsize>(flat.size()));
    if (array && !flat.empty())
        env->SetLongArrayRegion(array, 0, static_cast<jsize>(flat.size()), flat.data());
    return array;
}

// Returns the raw value, or Long.MIN_VALUE if the address cannot be read.
JNIEXPORT jlong JNICALL
Java_org_vita3k_emulator_CheatNative_readValue(JNIEnv *, jclass, jlong address, jint type) {
    auto *emuenv = active_env();
    int32_t value = 0;
    if (!emuenv || !cheat::read_value(emuenv->mem, static_cast<uint32_t>(address), value, cheat::type_size(type)))
        return INT64_MIN;
    return static_cast<jlong>(value);
}

JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_CheatNative_writeValue(JNIEnv *, jclass, jlong address, jint raw, jint type) {
    auto *emuenv = active_env();
    return emuenv && cheat::write_value(emuenv->mem, static_cast<uint32_t>(address), raw, cheat::type_size(type)) ? JNI_TRUE : JNI_FALSE;
}

// One line per item: id \t psv(0/1) \t enabled(0/1) \t supported(0/1) \t address \t type \t raw value \t info \t name
JNIEXPORT jstring JNICALL
Java_org_vita3k_emulator_CheatNative_cheatList(JNIEnv *env, jclass) {
    auto *emuenv = active_env();
    std::string text;
    if (emuenv) {
        ensure_loaded(*emuenv);
        for (const auto &item : cheats().items()) {
            text += std::to_string(item.id) + '\t' + (item.psv ? '1' : '0') + '\t' + (item.enabled ? '1' : '0') + '\t' + (item.supported ? '1' : '0') + '\t' + std::to_string(item.address) + '\t' + std::to_string(item.type) + '\t' + std::to_string(item.value) + '\t' + clean_text(item.info) + '\t' + clean_text(item.name) + '\n';
        }
    }
    return env->NewStringUTF(text.c_str());
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_cheatSetManual(JNIEnv *env, jclass, jlong address, jint raw, jint type, jboolean enabled, jstring name) {
    auto *emuenv = active_env();
    if (!emuenv)
        return;
    ensure_loaded(*emuenv);
    std::string text;
    if (name) {
        const char *chars = env->GetStringUTFChars(name, nullptr);
        if (chars) {
            text = clean_text(chars);
            env->ReleaseStringUTFChars(name, chars);
        }
    }
    cheats().set_manual(static_cast<uint32_t>(address), raw, type, enabled == JNI_TRUE, text);
    autosave(*emuenv);
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_cheatSetEnabled(JNIEnv *, jclass, jlong id, jboolean enabled) {
    auto *emuenv = active_env();
    if (!emuenv)
        return;
    ensure_loaded(*emuenv);
    cheats().set_enabled(id, enabled == JNI_TRUE);
    autosave(*emuenv);
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_cheatRemove(JNIEnv *, jclass, jlong id) {
    auto *emuenv = active_env();
    if (!emuenv)
        return;
    ensure_loaded(*emuenv);
    cheats().remove(id);
    autosave(*emuenv);
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_cheatSetAll(JNIEnv *, jclass, jboolean enabled) {
    auto *emuenv = active_env();
    if (!emuenv)
        return;
    ensure_loaded(*emuenv);
    cheats().set_all_enabled(enabled == JNI_TRUE);
    autosave(*emuenv);
}

// Re-reads the list file and the .psv file from disk; returns the number of items, -1 if no game runs.
JNIEXPORT jint JNICALL
Java_org_vita3k_emulator_CheatNative_cheatReload(JNIEnv *, jclass) {
    auto *emuenv = active_env();
    if (!emuenv)
        return -1;
    ensure_loaded(*emuenv);
    return cheats().reload(list_file(*emuenv), psv_file(*emuenv));
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_cheatSetPsvOffset(JNIEnv *, jclass, jlong offset) {
    auto *emuenv = active_env();
    if (!emuenv)
        return;
    ensure_loaded(*emuenv);
    cheats().set_psv_offset(static_cast<int32_t>(offset));
    autosave(*emuenv);
}

JNIEXPORT jlong JNICALL
Java_org_vita3k_emulator_CheatNative_cheatPsvOffset(JNIEnv *, jclass) {
    return static_cast<jlong>(cheats().psv_offset());
}

JNIEXPORT jstring JNICALL
Java_org_vita3k_emulator_CheatNative_cheatDir(JNIEnv *env, jclass) {
    auto *emuenv = active_env();
    const std::string dir = emuenv ? clean_text((emuenv->config_path / "cheats").string()) : std::string();
    return env->NewStringUTF(dir.c_str());
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_macroRecordStart(JNIEnv *, jclass) {
    macro::start_record();
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_macroRecordStop(JNIEnv *, jclass) {
    macro::stop_record();
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_macroPlayStart(JNIEnv *, jclass, jboolean loop) {
    macro::start_play(loop == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_CheatNative_macroPlayStop(JNIEnv *, jclass) {
    macro::stop_play();
}

JNIEXPORT jint JNICALL
Java_org_vita3k_emulator_CheatNative_macroMode(JNIEnv *, jclass) {
    return macro::get_mode();
}

JNIEXPORT jlong JNICALL
Java_org_vita3k_emulator_CheatNative_macroLength(JNIEnv *, jclass) {
    return static_cast<jlong>(macro::length());
}

} // extern "C"
