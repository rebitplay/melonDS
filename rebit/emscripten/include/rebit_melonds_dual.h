#ifndef REBIT_MELONDS_DUAL_H
#define REBIT_MELONDS_DUAL_H

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

enum
{
    REBIT_MELONDS_DUAL_API_VERSION = 1,
    REBIT_MELONDS_DUAL_WIDTH = 256,
    REBIT_MELONDS_DUAL_HEIGHT = 384,
    REBIT_MELONDS_DUAL_AUDIO_SAMPLE_RATE = 48000,
};

uint32_t md_api_version(void);
const char* md_runtime_abi(void);
const char* md_build_id(void);
const char* md_last_error(void);

int md_load(const uint8_t* rom, uint32_t rom_length, int players, uint32_t seed_low, uint32_t seed_high);
void md_destroy(void);
int md_is_loaded(void);
int md_player_count(void);

void md_set_visible_player(int player);
int md_visible_player(void);
int md_set_input(int player, uint32_t keys, int touching, int touch_x, int touch_y);
int md_run_frame(void);
uint32_t md_frame(int player);

const uint32_t* md_framebuffer(int player);
int md_width(void);
int md_height(void);

int md_audio_sample_rate(void);
int md_audio_available(void);
int md_audio_read(int maximum_frames);
const int16_t* md_audio_buffer(void);

uint32_t md_save_size(int player);
const uint8_t* md_save_data(int player);
int md_import_save(int player, const uint8_t* data, uint32_t length);

int md_export_checkpoint(void);
uint32_t md_checkpoint_size(void);
const uint8_t* md_checkpoint_data(void);
void md_clear_checkpoint(void);
int md_import_checkpoint(uint8_t* data, uint32_t length);

void md_set_scheduler_jitter(uint32_t profile);
int md_inject_desync_for_test(int player, uint32_t offset, uint32_t value);
uint32_t md_state_hash(void);

uint32_t md_mp_packets_sent(int player);
uint32_t md_mp_packets_received(int player);
uint32_t md_mp_commands(int player);
uint32_t md_mp_replies(int player);
double md_last_frame_ms(void);

#ifdef REBIT_MELONDS_ROLLBACK
/* Experimental, process-local state ring. These are not network import APIs.
 * Returns total serialized bytes per slot, or zero on failure.
 * A slot must be loaded with the exact frame tag returned by md_frame(). */
uint32_t md_rollback_configure(uint32_t capacity);
int md_rollback_save_slot(uint32_t slot, uint32_t frame);
int md_rollback_load_slot(uint32_t slot, uint32_t frame);
uint32_t md_rollback_part_size(uint32_t slot, uint32_t part);
const uint8_t* md_rollback_part_data(uint32_t slot, uint32_t part);
#endif

#if defined(__cplusplus)
}
#endif

#endif
