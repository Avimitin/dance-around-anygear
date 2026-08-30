// SPDX-License-Identifier: GPL-3.0-or-later
// Late-loaded Unity-shaped module used to exercise loader notifications.

#include <windows.h>

extern "C" __declspec(dllexport) HMODULE anygearFakeLoadVp4u() {
    return LoadLibraryW(L"visionposewrapper.dll");
}

extern "C" __declspec(dllexport) FARPROC anygearFakeResolveVp4u(
    HMODULE module) {
    return GetProcAddress(module, "vp4uGetApiTable");
}
