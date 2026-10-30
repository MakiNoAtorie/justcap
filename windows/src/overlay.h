#pragma once
#include <windows.h>

inline DWORD regGet(const wchar_t* name, DWORD fallback) {
    DWORD v, size = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\justcap", name, RRF_RT_REG_DWORD, NULL, &v, &size) == ERROR_SUCCESS ? v : fallback;
}

inline void regSet(const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\justcap", name, REG_DWORD, &v, sizeof v);
}
