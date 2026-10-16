#ifndef SIMULATOR_ZX128_H
#define SIMULATOR_ZX128_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
//   CORE EMULATOR CONTROLS & TIMING INTERFACES
// ============================================================================

/**
 * Initializes the ZX Spectrum 128K core, partitions memory maps, resets variables,
 * and loads the binary ROM image segments into internal emulated space.
 */
void zx128_init(const char *rom1, const char *rom2, const char *trdos);

/**
 * Executes exactly one 50Hz hardware video frame (69,888 CPU T-states), handles
 * internal line interrupts, fires synchronous sound generation, and builds the display frame buffer.
 */
void zx128_run_frame(void);

/**
 * Performs a complete hardware state clear (Hard Reset), resetting memory locks,
 * system ports, CPU state vectors, variables, and attached storage states.
 */
void zx128_reset(void);


// ============================================================================
//   VIDEO DISPLAY FRAMEBUFFER EXTRACTION
// ============================================================================

/**
 * Retrieves a direct pointer to the underlying continuous linear RGBA display buffer.
 * Dimension size is fixed at 320x256 pixels (incorporating core screen + active borders).
 * @return Continuous pointer to linear array of 32-bit pixel color elements (DWORDs).
 */
const uint32_t* zx48_get_pixel_buffer(void);


// ============================================================================
//   INPUT PERIPHERALS & KEYBOARD MATRIX INJECTION
// ============================================================================

/**
 * Direct hardware state injector mapping physical platform events onto the emulated
 * 8x5 keyboard matrix registers of the ZX Spectrum ULA.
 * @param row The keyboard row index identifier (Range: 0 through 7).
 * @param bit The exact bit offset position within specified target row (Range: 0 through 4).
 * @param isPressed Boolean value asserting if the key is pressed (true) or released (false).
 */
void zx128_press_key(int row, int bit, bool isPressed);


// ============================================================================
//   AUDIO SYNTHESIS SYSTEM & BUFFERING OUTPUT
// ============================================================================

/**
 * Extraction callback invoked by host audio engines (such as AudioQueue or DirectSound) 
 * to pull synthetic interleaved stereo sound data directly from the emulator's ring buffers.
 * @param buffer Continuous output container destination for audio chunks.
 * @param sample_count total chunk frame sizes to process (Outputs left/right data channels sequentially).
 */
void zx128_get_audio_samples(int16_t *buffer, int sample_count);


// ============================================================================
//   FILE SYSTEM STORAGE INJECTION ARCHITECTURES (SNAPSHOTS / FILES)
// ============================================================================

/**
 * Loads and injects static .SNA memory configurations (Both standard 48K formats and 128K models).
 * @param filepath Full host system path pointing to targeted binary snapshot.
 * @return True on parse success, false if file structure is corrupt or inaccessible.
 */
bool zx128_load_sna(const char *filepath);

/**
 * Extracts, uncompresses, and runs active .Z80 RLE structural snapshots (Versions v1, v2, and v3).
 * @param filepath System target path string containing the active snapshot archive.
 * @return True on successful decompression and initialization, false on structural failure.
 */
bool zx128_load_z80(const char *filepath);

/**
 * Mounts standard structural tape vector binary loops (.TAP containers) straight into internal hooks.
 * @param filepath System path targeting raw storage container.
 * @return True if resource successfully locked and mapped, false if parsing aborted.
 */
bool zx128_load_tape(const char *filepath);

/**
 * Mounts binary storage disks (.TRD images) onto the system emulated VG93 controller interface.
 * @param filepath System file path identifying target container element.
 * @return True if file size bounds check passes and resource locks, false if failed.
 */
bool zx128_load_trd(const char *filepath);

/**
 * Opens a Sinclair catalog (.SCL resource), extracts embedded track/sector variables on-the-fly,
 * builds a complete geometric .TRD disk structure dynamically inside memory, and boots it via TR-DOS.
 * @param filepath Target container platform string location.
 * @return True if conversion validation passes, false if headers are corrupted.
 */
bool zx128_load_scl(const char *filepath);

#ifdef __cplusplus
}
#endif

#endif /* SIMULATOR_ZX128_H */
