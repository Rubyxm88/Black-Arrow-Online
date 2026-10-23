/*
 * xemu: the Xbox's audio setting in the console EEPROM
 *
 * The 32-bit word at 0x98: low byte 0 stereo, 1 mono, 2 Dolby Surround;
 * 0x10000 Dolby Digital, 0x20000 DTS. The Sound menu sets it, so players can
 * switch between Accurate (needs Stereo) and 5.1 (needs Dolby) without the
 * Xbox dashboard.
 */

#ifndef HW_XBOX_AUDIO_SETTING_H
#define HW_XBOX_AUDIO_SETTING_H

#include <stdbool.h>
#include <stdint.h>

#define XBOX_AUDIO_STEREO 0
#define XBOX_AUDIO_DOLBY_SURROUND 2
#define XBOX_AUDIO_AC3 0x10000
#define XBOX_AUDIO_DTS 0x20000
/* The bits xemu changes: the mode byte, Dolby Digital and DTS */
#define XBOX_AUDIO_SETTING_MASK 0x300FFu

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The setting now (what the Xbox's next start uses) and the one it started
 * with. False when there is no EEPROM yet.
 */
bool xbox_audio_setting_get(uint32_t *now, uint32_t *booted);

/*
 * Change it in the running EEPROM and its file, fixing the checksum. The Xbox
 * uses it from its next start.
 */
bool xbox_audio_setting_set(uint32_t flags);

/* Dolby Surround, Dolby Digital or DTS */
static inline bool xbox_audio_setting_is_dolby(uint32_t flags)
{
    return (flags & 0xFF) == XBOX_AUDIO_DOLBY_SURROUND ||
           (flags & (XBOX_AUDIO_AC3 | XBOX_AUDIO_DTS));
}

#ifdef __cplusplus
}
#endif

#endif
