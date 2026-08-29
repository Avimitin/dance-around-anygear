// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal, append-only declaration of the Spice SDK v0 ABI used by Anygear.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef enum ANYGEAR_SPICE_STATUS {
    ANYGEAR_SPICE_SUCCESS = 0,
    ANYGEAR_SPICE_GENERIC_ERROR = 1,
    ANYGEAR_SPICE_NOT_INITIALIZED = 2,
    ANYGEAR_SPICE_NOT_SUPPORTED = 3,
    ANYGEAR_SPICE_TOO_SMALL = 4,
    ANYGEAR_SPICE_TOO_LATE = 5,
    ANYGEAR_SPICE_INVALID_ARGUMENT_1 = 1001,
    ANYGEAR_SPICE_INVALID_ARGUMENT_2 = 1002,
    ANYGEAR_SPICE_INVALID_ARGUMENT_3 = 1003,
} ANYGEAR_SPICE_STATUS;

typedef enum ANYGEAR_SPICE_LOG_LEVEL {
    ANYGEAR_SPICE_LOG_MISC = 0,
    ANYGEAR_SPICE_LOG_INFO = 1,
    ANYGEAR_SPICE_LOG_WARNING = 2,
    ANYGEAR_SPICE_LOG_FATAL = 3,
} ANYGEAR_SPICE_LOG_LEVEL;

typedef enum ANYGEAR_SPICE_TOAST_SEVERITY {
    ANYGEAR_SPICE_TOAST_INFO = 0,
    ANYGEAR_SPICE_TOAST_SUCCESS = 1,
    ANYGEAR_SPICE_TOAST_WARNING = 2,
    ANYGEAR_SPICE_TOAST_ERROR = 3,
} ANYGEAR_SPICE_TOAST_SEVERITY;

typedef struct ANYGEAR_SPICE_TOUCH_POINT {
    uint32_t id;
    int x;
    int y;
} ANYGEAR_SPICE_TOUCH_POINT;

typedef struct ANYGEAR_SPICE_GAME_INFO {
    char name[64];
} ANYGEAR_SPICE_GAME_INFO;

typedef struct ANYGEAR_SPICE_AVS_INFO {
    char model[4];
    char dest;
    char spec;
    char rev;
    char ext[11];
} ANYGEAR_SPICE_AVS_INFO;

typedef ANYGEAR_SPICE_STATUS (__cdecl *anygear_spice_log_func)(
    ANYGEAR_SPICE_LOG_LEVEL level, const char *module, const char *message);
typedef ANYGEAR_SPICE_STATUS (__cdecl *anygear_spice_get_game_info_func)(
    ANYGEAR_SPICE_GAME_INFO *info);
typedef ANYGEAR_SPICE_STATUS (__cdecl *anygear_spice_get_avs_info_func)(
    ANYGEAR_SPICE_AVS_INFO *info);
typedef ANYGEAR_SPICE_STATUS (__cdecl *anygear_spice_get_button_func)(
    uint32_t id, bool *pressed, float *velocity);
typedef ANYGEAR_SPICE_STATUS (__cdecl *anygear_spice_set_button_func)(
    uint32_t id, bool pressed, float velocity);
typedef ANYGEAR_SPICE_STATUS (__cdecl *anygear_spice_get_analog_func)(
    uint32_t id, float *value);
typedef ANYGEAR_SPICE_STATUS (__cdecl *anygear_spice_set_analog_func)(
    uint32_t id, bool override_active, float value);
typedef ANYGEAR_SPICE_STATUS (__cdecl *anygear_spice_get_light_func)(
    uint32_t id, float *value);
typedef ANYGEAR_SPICE_STATUS (__cdecl *anygear_spice_set_light_func)(
    uint32_t id, bool override_active, float value);
typedef ANYGEAR_SPICE_STATUS (__cdecl *anygear_spice_set_touch_func)(
    const ANYGEAR_SPICE_TOUCH_POINT *points, uint32_t count);
typedef ANYGEAR_SPICE_STATUS (__cdecl *anygear_spice_clear_touch_func)(
    const uint32_t *ids, uint32_t count);
typedef ANYGEAR_SPICE_STATUS (__cdecl *anygear_spice_insert_card_func)(
    uint8_t unit, const char *card_id);
typedef ANYGEAR_SPICE_STATUS (__cdecl *anygear_spice_set_keypad_func)(
    uint8_t unit, char key);
typedef ANYGEAR_SPICE_STATUS (__cdecl *anygear_spice_add_toast_func)(
    ANYGEAR_SPICE_TOAST_SEVERITY severity, const char *text);

// Proposed append-only v0.3 helper. It maps an exact LoadLibrary name to an
// already loaded module through Spice's existing libraryhook implementation.
// Older Spice builds zero this field because the caller-supplied structure is
// larger than the version they know; callers must therefore null-check it.
typedef ANYGEAR_SPICE_STATUS (__cdecl *anygear_spice_hook_library_func)(
    const char *library_name, void *module);

typedef struct ANYGEAR_SPICE_SDK_V0 {
    uint32_t size;
    anygear_spice_log_func log;
    anygear_spice_get_game_info_func get_game_info;
    anygear_spice_get_avs_info_func get_avs_info;
    anygear_spice_get_button_func get_button;
    anygear_spice_set_button_func set_button;
    anygear_spice_get_analog_func get_analog;
    anygear_spice_set_analog_func set_analog;
    anygear_spice_get_light_func get_light;
    anygear_spice_set_light_func set_light;
    anygear_spice_set_touch_func set_touch;
    anygear_spice_clear_touch_func clear_touch;
    anygear_spice_insert_card_func insert_card;
    anygear_spice_set_keypad_func set_keypad;
    anygear_spice_add_toast_func add_toast;
    anygear_spice_hook_library_func hook_library;
} ANYGEAR_SPICE_SDK_V0;

typedef void (__cdecl *anygear_spice_destroy_callback_func)(void);
typedef ANYGEAR_SPICE_STATUS (__cdecl *anygear_spice_init_func)(
    uint32_t version,
    anygear_spice_destroy_callback_func destroy_callback,
    void *sdk_functions);

#ifdef __cplusplus
}
#endif
