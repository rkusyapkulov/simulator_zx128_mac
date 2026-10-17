#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include "z80.h"

// --- СОВМЕСТИМОСТЬ С СИ-СТРУКТУРОЙ ИЗ Z80.C ---
typedef uint8_t  BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;

#define CYCLES_PER_STEP         (Z80_CPU_SPEED / 50)

// --- ПАРАМЕТРЫ ЭКРАНА ZX SPECTRUM ---
#define BORDER_WIDTH  32
#define BORDER_HEIGHT 24
#define SCREEN_WIDTH  (256 + BORDER_WIDTH * 2)  // 320
#define SCREEN_HEIGHT (192 + BORDER_HEIGHT * 2) // 240

// --- ГЛОБАЛЬНАЯ ПАМЯТЬ ZX SPECTRUM 128K + TR-DOS ---
BYTE spec_rom[2][16384];  // ROM 0: 128К Меню/Editor, ROM 1: 48K BASIC
BYTE spec_ram[8][16384];  // 8 страниц ОЗУ по 16 Кб (Всего 128 Кб)
BYTE trdos_rom[16384];    // Самостоятельное ПЗУ системы TR-DOS

int current_ram_page = 0;  // Переключаемая страница ОЗУ в окне 0xC000-0xFFFF
int current_rom_page = 0;  // Текущая страница ПЗУ в окне 0x0000-0x3FFF
bool screen_shadow_page = false; // true = экран в RAM 7, false = RAM 5
bool memory_lock_128k = false;   // Блокировка режима 128К (Out 0x7FFD больше не работает)

bool trdos_active = false; // Флаг: активно ли сейчас ПЗУ TR-DOS вместо обычного ROM

BYTE spec_key_rows[8] = { 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F }; 
BYTE physical_key_rows[8] = { 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F }; 
BYTE gui_key_rows[8] = { 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F }; 
int gui_key_frames[40] = {0};

static DWORD pixel_buffer[SCREEN_WIDTH * SCREEN_HEIGHT];

// --- ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ДЛЯ ЭМУЛЯЦИИ ЛЕНТЫ ---
BYTE* current_tap_data = NULL;      // Динамический буфер текущего TAP-файла
size_t current_tap_size = 0;        // Размер данных TAP
size_t tap_current_pos = 0;         // Текущая позиция чтения в TAP
bool tap_is_loaded = false;         // Флаг, загружена ли "лента" в эмулятор

// --- ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ДЛЯ ЭМУЛЯЦИИ ДИСКОВОДА TR-DOS ---
BYTE* current_trd_data = NULL;      // Буфер сырого образа дискеты .TRD
size_t current_trd_size = 0;        // Размер данных TRD
bool trd_is_inserted = false;       // Флаг: вставлен ли диск в дисковод A:

bool spec_speaker_state = false;
int spec_border_color = 0;

bool spec_flash_state = false;
BYTE spec_flash_frame = 0;

bool spec_ear_signal = false;

// ============================================================================
//   ОПТИМИЗИРОВАННЫЙ СИНХРОННЫЙ АУДИОДВИЖОК 44100 Гц (ПЕРЕНОС ИЗ ZX48)
// ============================================================================
#define CPU_CLOCK_HZ       3546900ULL  // Частота Z80 для Spectrum 128K (70908 тактов на кадр * 50 Гц)
#define AY_CLOCK_HZ        1773450ULL  // Частота AY музыкального чипа
#define AUDIO_SAMPLE_RATE  44100ULL

static uint64_t cpu_tstates_total = 0;
static uint64_t audio_cursor_tstates = 0;
static uint64_t ay_clock_phase = 0;
static uint64_t audio_sample_phase = 0;
static uint8_t noise_prescaler = 0;
static uint8_t envelope_prescaler = 0;

typedef enum {
    EVENT_AY_REG_SELECT,
    EVENT_AY_REG_WRITE,
    EVENT_BEEPER_WRITE
} AudioEventType;

typedef struct {
    AudioEventType type;
    uint64_t tstate;
    uint8_t value;
} AudioEvent;

#define AUDIO_EVENT_CAPACITY 32768
static AudioEvent audio_event_buffer[AUDIO_EVENT_CAPACITY];
static uint32_t audio_event_head = 0;
static uint32_t audio_event_tail = 0;

void QueueAudioEvent(AudioEventType type, uint8_t value) {
    uint32_t next_head = (audio_event_head + 1) % AUDIO_EVENT_CAPACITY;
    if (next_head != audio_event_tail) {
        audio_event_buffer[audio_event_head].type = type;
        audio_event_buffer[audio_event_head].tstate = cpu_tstates_total;
        audio_event_buffer[audio_event_head].value = value;
        audio_event_head = next_head;
    }
}

enum StereoMode { MODE_MONO, MODE_ABC, MODE_ACB };

struct AY_State {
    uint8_t regs[16];
    uint8_t selected_reg;
    enum StereoMode stereo_mode;
    
    int32_t tone_counters[3];
    uint8_t tone_outputs[3];
    
    int32_t noise_counter;
    uint32_t noise_shift_reg;
    uint8_t noise_output;

    int16_t chan_volume[3];
} ay;

struct AY_Envelope {
    uint32_t count;
    int32_t step;               // 15..0
    uint8_t attack_mask;        // 0x00 или 0x0F
    uint8_t holding;
} ay_env;

static uint8_t local_ay_selected_reg = 0;
static uint8_t local_beeper_bit = 0;

#define AUDIO_RING_SIZE 16384
static int16_t audio_ring_buffer[AUDIO_RING_SIZE];
static volatile uint32_t audio_ring_head = 0;
static volatile uint32_t audio_ring_tail = 0;

void audio_ring_push(int16_t sample) {
    uint32_t head = audio_ring_head;
    uint32_t next_head = (head + 1u) % AUDIO_RING_SIZE;
    uint32_t tail = audio_ring_tail;
    if (next_head != tail) {
        audio_ring_buffer[head] = sample;
        audio_ring_head = next_head;
    }
}

void zx128_get_audio_samples(int16_t *buffer, int sample_count) {
    if (!buffer || sample_count <= 0) return;
    for (int i = 0; i < sample_count; i++) {
        uint32_t tail = audio_ring_tail;
        uint32_t head = audio_ring_head;
        if (tail != head) {
            buffer[i] = audio_ring_buffer[tail];
            audio_ring_tail = (tail + 1u) % AUDIO_RING_SIZE;
        } else {
            buffer[i] = 0;
        }
    }
}

void ClearAudioBuffers(void) {
    audio_event_head = 0;
    audio_event_tail = 0;
    audio_ring_head = 0;
    audio_ring_tail = 0;
    local_beeper_bit = 0;
    ay.chan_volume[0] = 0; ay.chan_volume[1] = 0; ay.chan_volume[2] = 0;
}

static void ay_clock_tick(void) {
    for (int chan = 0; chan < 3; chan++) {
        uint32_t period = ((uint32_t)(ay.regs[chan * 2 + 1] & 0x0F) << 8) | ay.regs[chan * 2];
        if (period == 0) period = 1;
        if (++ay.tone_counters[chan] >= (int32_t)period) {
            ay.tone_counters[chan] -= (int32_t)period;
            ay.tone_outputs[chan] ^= 1;
        }
    }

    if (++noise_prescaler >= 2) {
        noise_prescaler = 0;
        uint32_t noise_period = ay.regs[6] & 0x1F;
        if (noise_period == 0) noise_period = 1;
        if (++ay.noise_counter >= (int32_t)noise_period) {
            ay.noise_counter = 0;
            ay.noise_shift_reg = (ay.noise_shift_reg >> 1) | (((ay.noise_shift_reg ^ (ay.noise_shift_reg >> 3)) & 1) << 16);
            ay.noise_output = (uint8_t)(ay.noise_shift_reg & 1u);
        }
    }

    if (++envelope_prescaler >= 2) {
        envelope_prescaler = 0;
        uint32_t env_period = ((uint32_t)ay.regs[12] << 8) | ay.regs[11];
        if (env_period == 0) env_period = 1;
        if (!ay_env.holding && ++ay_env.count >= env_period) {
            ay_env.count = 0;
            --ay_env.step;
            if (ay_env.step < 0) {
                uint8_t shape = ay.regs[13] & 0x0F;
                if ((shape & 0x08) == 0) {
                    ay_env.attack_mask = 0;
                    ay_env.step = 0;
                    ay_env.holding = 1;
                } else if (shape & 0x01) {
                    if (shape & 0x02) ay_env.attack_mask ^= 0x0F;
                    ay_env.step = 0;
                    ay_env.holding = 1;
                } else {
                    if (shape & 0x02) ay_env.attack_mask ^= 0x0F;
                    ay_env.step = 15;
                }
            }
        }
    }
}

static void update_cached_volume(int chan) {
    static const int16_t volume_table[16] = {
        0, 200, 290, 420, 610, 880, 1270, 1840,
        2660, 3850, 5570, 8060, 11660, 16860, 24400, 32767
    };
    uint8_t vol = ay.regs[8 + chan];
    if (vol & 0x10) {
        ay.chan_volume[chan] = -1;
    } else {
        ay.chan_volume[chan] = volume_table[vol & 0x0F] / 3;
    }
}

static void apply_due_audio_events(void) {
    while (audio_event_tail != audio_event_head) {
        AudioEvent *evt = &audio_event_buffer[audio_event_tail];
        if (evt->tstate > audio_cursor_tstates) break;

        if (evt->type == EVENT_AY_REG_SELECT) {
            local_ay_selected_reg = evt->value & 0x0F;
        } else if (evt->type == EVENT_AY_REG_WRITE) {
            ay.regs[local_ay_selected_reg] = evt->value;
            if (local_ay_selected_reg >= 8 && local_ay_selected_reg <= 10) {
                update_cached_volume(local_ay_selected_reg - 8);
            } else if (local_ay_selected_reg == 7) {
                update_cached_volume(0); update_cached_volume(1); update_cached_volume(2);
            } else if (local_ay_selected_reg == 13) {
                uint8_t shape = evt->value & 0x0F;
                ay_env.count = 0;
                ay_env.step = 15;
                ay_env.attack_mask = (shape & 0x04) ? 0x0F : 0x00;
                ay_env.holding = 0;
            }
        } else if (evt->type == EVENT_BEEPER_WRITE) {
            local_beeper_bit = evt->value ? 1 : 0;
        }
        audio_event_tail = (audio_event_tail + 1u) % AUDIO_EVENT_CAPACITY;
    }
}

static int16_t mix_audio_sample(void) {
    static const int16_t volume_table[16] = {
        0, 200, 290, 420, 610, 880, 1270, 1840,
        2660, 3850, 5570, 8060, 11660, 16860, 24400, 32767
    };
    uint8_t mixer = ay.regs[7];
    double ch_amp[3] = {0.0, 0.0, 0.0};

    for (int ch = 0; ch < 3; ch++) {
        if (((mixer & (1 << ch)) || ay.tone_outputs[ch]) &&
            ((mixer & (8 << ch)) || ay.noise_output)) {
            int16_t v = ay.chan_volume[ch];
            ch_amp[ch] = (v >= 0) ? v : (volume_table[(ay_env.step ^ ay_env.attack_mask) & 0x0F] / 3);
        }
    }

    // Полноценное панорамирование музыкальных каналов (Стерео ABC -> Моно микс)
    double left_mix, right_mix;
    switch (ay.stereo_mode) {
        case MODE_ABC:
            left_mix = ch_amp[0] + ch_amp[1] * 0.5;
            right_mix = ch_amp[2] + ch_amp[1] * 0.5;
            break;
        case MODE_ACB:
            left_mix = ch_amp[0] + ch_amp[2] * 0.5;
            right_mix = ch_amp[1] + ch_amp[2] * 0.5;
            break;
        case MODE_MONO:
        default:
            left_mix = (ch_amp[0] + ch_amp[1] + ch_amp[2]) * 0.5;
            right_mix = left_mix;
            break;
    }

    int32_t ay_mixed_out = (int32_t)((left_mix + right_mix) * 0.5);
    int32_t sample = ay_mixed_out + (local_beeper_bit ? 4000 : 0);
    
    if (sample > 32767) sample = 32767;
    if (sample < -32768) sample = -32768;
    return (int16_t)sample;
}

void zx128_tick_audio(uint32_t cycles) {
    if (ay.noise_shift_reg == 0) ay.noise_shift_reg = 1;
    uint64_t target_tstates = cpu_tstates_total;
    (void)cycles;

    while (audio_cursor_tstates < target_tstates) {
        apply_due_audio_events();

        ay_clock_phase += AY_CLOCK_HZ;
        const uint64_t ay_clock_threshold = CPU_CLOCK_HZ * 8ULL;
        while (ay_clock_phase >= ay_clock_threshold) {
            ay_clock_phase -= ay_clock_threshold;
            ay_clock_tick();
        }

        ++audio_cursor_tstates;
        audio_sample_phase += AUDIO_SAMPLE_RATE;
        if (audio_sample_phase >= CPU_CLOCK_HZ) {
            audio_sample_phase -= CPU_CLOCK_HZ;
            audio_ring_push(mix_audio_sample());
        }
    }
    apply_due_audio_events();
}

void AY_Reset(void) {
    memset(ay.regs, 0, sizeof(ay.regs));
    ay.selected_reg = 0;
    local_ay_selected_reg = 0;
    
    memset(ay.tone_counters, 0, sizeof(ay.tone_counters));
    memset(ay.tone_outputs, 1, sizeof(ay.tone_outputs));
    ay.noise_counter = 0;
    ay.noise_shift_reg = 1;
    ay.noise_output = 1;
    
    ay.chan_volume[0] = 0; ay.chan_volume[1] = 0; ay.chan_volume[2] = 0;
    
    ay_env.count = 0;
    ay_env.step = 0;
    ay_env.attack_mask = 0;
    ay_env.holding = 1;
}

enum TransferType {
    TRANSFER_NONE,
    TRANSFER_READ_SECTOR,
    TRANSFER_WRITE_SECTOR,
    TRANSFER_READ_ADDRESS,
    TRANSFER_WRITE_TRACK   
};

#define WRITE_TRACK_BYTES 6250
#define DISK_REV_TSTATES  700000

struct VG93 {
    BYTE track_reg;
    BYTE sector_reg;
    BYTE data_reg;
    BYTE sector_buffer[256];
    int  transfer_pos;
    int  transfer_len;   
    bool last_cmd_type1;
    bool multi_sector;    
    bool record_not_found; 
    BYTE system_reg;
    BYTE current_id_sector;
    bool step_direction_in;

    bool busy;
    bool drq;
    bool intrq;
    bool lost_data;   
    uint64_t drq_since;

    enum TransferType transfer_type;
};

static struct VG93 vg93;

void VG93_ResetChip(void) {
    vg93.track_reg = 0;
    vg93.sector_reg = 1;
    vg93.data_reg = 0;
    vg93.transfer_pos = -1;
    vg93.transfer_len = 0;
    vg93.last_cmd_type1 = true;
    vg93.multi_sector = false;
    vg93.record_not_found = false;
    vg93.transfer_type = TRANSFER_NONE;
    vg93.current_id_sector = 1;
    vg93.step_direction_in = true;

    vg93.busy = false;
    vg93.drq = false;
    vg93.intrq = true;
    vg93.lost_data = false;
    vg93.drq_since = 0;
}

void VG93_Reset(void) {
    VG93_ResetChip();
    vg93.system_reg = 0x00;
}

void VG93_EndCommand(bool rnf) {
    vg93.transfer_pos = -1;
    vg93.transfer_len = 0;
    vg93.transfer_type = TRANSFER_NONE;
    vg93.multi_sector = false;
    vg93.record_not_found = rnf;

    vg93.busy = false; 
    vg93.drq = false;
    vg93.intrq = true;
}

bool VG93_DiskReady(void) {
    return trd_is_inserted && current_trd_data != NULL;
}

int VG93_Side(void) {
    return 1 - ((vg93.system_reg >> 4) & 1);
}

size_t VG93_GetTRDOffset(void) {
    size_t logical_track = (size_t)vg93.track_reg * 2 + VG93_Side();
    size_t sector = (size_t)(vg93.sector_reg - 1);
    return (logical_track * 16 + sector) * 256;
}

bool VG93_LoadCurrentSector(void) {
    if (!VG93_DiskReady()) return false;
    if (vg93.sector_reg < 1 || vg93.sector_reg > 16) return false;
    size_t offset = VG93_GetTRDOffset();
    if (offset + 256 > current_trd_size) return false;
    memcpy(vg93.sector_buffer, &current_trd_data[offset], 256);
    vg93.current_id_sector = vg93.sector_reg;
    return true;
}

void VG93_CheckDrqTimeout() {
    if (!vg93.drq || vg93.transfer_type == TRANSFER_NONE) return;
    const uint64_t DRQ_TIMEOUT_TSTATES = 50000;   
    const uint64_t now = cpu_tstates_total;
    if (now < vg93.drq_since) { vg93.drq_since = now; return; }   
    if (now - vg93.drq_since < DRQ_TIMEOUT_TSTATES) return;

    vg93.lost_data = true;

    if (vg93.transfer_type == TRANSFER_READ_SECTOR && vg93.multi_sector) {
        ++vg93.sector_reg;
        if (vg93.sector_reg > 16 || !VG93_LoadCurrentSector()) {
            VG93_EndCommand(true);          
        }
        else {
            vg93.transfer_pos = 0;
            vg93.transfer_len = 256;
            vg93.drq = true;
            vg93.drq_since = now;
        }
    }
    else {
        VG93_EndCommand(false);   
    }
}

BYTE VG93_PhysicalSectorId(int slot) {
    static const BYTE order[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
    return order[slot & 15];
}

BYTE VG93_NextIdSector(void) {
    const uint64_t pos = cpu_tstates_total % DISK_REV_TSTATES;
    const int slot = (int)(pos * 16 / DISK_REV_TSTATES);
    return VG93_PhysicalSectorId(slot + 1);
}

WORD VG93_Crc16Ccitt(const BYTE* p, int n) {      
    WORD crc = 0xFFFF;
    for (int i = 0; i < n; ++i) {
        crc ^= (WORD)p[i] << 8;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? (WORD)((crc << 1) ^ 0x1021) : (WORD)(crc << 1);
    }
    return crc;
}

BYTE VG93_ReadStatus() {
    VG93_CheckDrqTimeout();
    BYTE status = 0;
    if (!VG93_DiskReady()) status |= 0x80;
    if (!vg93.last_cmd_type1 && vg93.record_not_found) status |= 0x10;
    if (!vg93.last_cmd_type1 && vg93.lost_data) status |= 0x04;   
    if (vg93.last_cmd_type1 && vg93.track_reg == 0) status |= 0x04;

    if (vg93.transfer_type != TRANSFER_NONE && vg93.transfer_pos >= 0 && vg93.transfer_pos < vg93.transfer_len) {
        status |= 0x03; 
    }
    else if (vg93.last_cmd_type1 && VG93_DiskReady()) {
        if ((cpu_tstates_total % DISK_REV_TSTATES) < 35000)
            status |= 0x02;
    }
    vg93.intrq = false; 
    return status;
}

BYTE VG93_ReadTrack() {
    return vg93.track_reg;
}

BYTE VG93_ReadSector() {
    return vg93.sector_reg;
}

BYTE VG93_ReadData() {
    if (vg93.transfer_type == TRANSFER_READ_SECTOR || vg93.transfer_type == TRANSFER_READ_ADDRESS) {
        if (vg93.transfer_pos >= 0 && vg93.transfer_pos < vg93.transfer_len) {
            vg93.data_reg = vg93.sector_buffer[vg93.transfer_pos++];
            vg93.drq_since = cpu_tstates_total;
            vg93.drq = (vg93.transfer_pos < vg93.transfer_len); 

            if (vg93.transfer_pos >= vg93.transfer_len) {
                if (vg93.multi_sector && vg93.transfer_type == TRANSFER_READ_SECTOR) {
                    vg93.sector_reg++;
                    if (vg93.sector_reg > 16) {
                        VG93_EndCommand(true);
                    }
                    else if (VG93_LoadCurrentSector()) {
                        vg93.transfer_pos = 0;
                        vg93.transfer_len = 256;
                        vg93.drq = true;
                    }
                    else {
                        VG93_EndCommand(true);
                    }
                }
                else {
                    VG93_EndCommand(false);
                }
            }
        }
        return vg93.data_reg;
    }
    return vg93.data_reg;
}

BYTE VG93_ReadSystem() {
    VG93_CheckDrqTimeout();
    BYTE sys = vg93.system_reg & 0x3F;
    if (vg93.drq) sys |= 0x40; 
    if (vg93.intrq) sys |= 0x80; 
    return sys;
}

bool VG93_FlushCurrentSector(void) {
    if (!VG93_DiskReady() || vg93.sector_reg < 1 || vg93.sector_reg > 16) return false;
    size_t offset = VG93_GetTRDOffset();
    if (offset + 256 > current_trd_size) return false;
    memcpy(&current_trd_data[offset], vg93.sector_buffer, 256);
    return true;
}

bool VG93_FormatCurrentTrack(void) {
    if (!VG93_DiskReady()) return false;
    const BYTE saved_sector = vg93.sector_reg;
    for (int s = 1; s <= 16; ++s) {
        vg93.sector_reg = (BYTE)s;
        size_t offset = VG93_GetTRDOffset();
        if (offset + 256 > current_trd_size) {
            vg93.sector_reg = saved_sector;
            return false;
        }
        memset(current_trd_data + offset, 0, 256);
    }
    vg93.sector_reg = saved_sector;
    return true;
}

bool VG93_FinishWriteTrack(void) {
    if (!VG93_DiskReady()) return false;
    return VG93_FormatCurrentTrack();
}

void VG93_WriteCommand(BYTE val) {
    vg93.drq = false;
    vg93.intrq = false;
    vg93.lost_data = false;
    vg93.drq_since = cpu_tstates_total;

    if ((val & 0xF0) == 0xD0) {
        vg93.last_cmd_type1 = true;
        vg93.transfer_pos = -1;
        vg93.transfer_len = 0;
        vg93.transfer_type = TRANSFER_NONE;
        vg93.multi_sector = false;
        vg93.record_not_found = false;
        vg93.busy = false;          
        vg93.intrq = true;          
        return;                
    }

    if ((val & 0x80) == 0) {
        vg93.last_cmd_type1 = true;
        vg93.busy = true;
        vg93.transfer_pos = -1;
        vg93.transfer_len = 0;
        vg93.transfer_type = TRANSFER_NONE;
        vg93.multi_sector = false;

        switch (val & 0xF0) {
        case 0x00: vg93.track_reg = 0; break;
        case 0x10: vg93.track_reg = vg93.data_reg; break;
        case 0x20:
        case 0x30:
            if (vg93.step_direction_in) { if (vg93.track_reg < 79) vg93.track_reg++; }
            else { if (vg93.track_reg > 0) vg93.track_reg--; }
            if (val & 0x10) vg93.data_reg = vg93.track_reg; 
            break;
        case 0x40:
        case 0x50:
            if (vg93.track_reg < 79) vg93.track_reg++;
            if (val & 0x10) vg93.data_reg = vg93.track_reg; 
            vg93.step_direction_in = true;
            break;
        case 0x60:
        case 0x70:
            if (vg93.track_reg > 0) vg93.track_reg--;
            if (val & 0x10) vg93.data_reg = vg93.track_reg; 
            vg93.step_direction_in = false;
            break;
        }
        VG93_EndCommand(false); 
        return;
    }

    if ((val & 0xC0) == 0x80) {
        vg93.last_cmd_type1 = false;
        vg93.busy = true; 
        vg93.multi_sector = (val & 0x10) != 0;
        vg93.transfer_pos = 0;
        vg93.transfer_len = 256;

        if ((val & 0x20) == 0) {
            if (VG93_LoadCurrentSector()) {
                vg93.transfer_type = TRANSFER_READ_SECTOR;
                vg93.drq = true; 
            } else {
                VG93_EndCommand(true); 
            }
        } else {
            if (VG93_DiskReady() && vg93.sector_reg >= 1 && vg93.sector_reg <= 16) {
                memset(vg93.sector_buffer, 0, 256);
                vg93.transfer_type = TRANSFER_WRITE_SECTOR;
                vg93.drq = true; 
            } else {
                VG93_EndCommand(true);
            }
        }
        return;
    }

    if ((val & 0xF0) == 0xC0) {
        vg93.last_cmd_type1 = false;
        vg93.busy = true;
        vg93.multi_sector = false;
        vg93.record_not_found = false;
        if (!VG93_DiskReady()) { VG93_EndCommand(true); return; }

        const BYTE id_track = vg93.track_reg;
        const BYTE id_side = (BYTE)VG93_Side();
        const BYTE id_sector = VG93_NextIdSector();

        vg93.sector_buffer[0] = id_track;
        vg93.sector_buffer[1] = id_side;
        vg93.sector_buffer[2] = id_sector;
        vg93.sector_buffer[3] = 1;
        const BYTE crc_src[8] = { 0xA1, 0xA1, 0xA1, 0xFE, id_track, id_side, id_sector, 1 };
        const WORD id_crc = VG93_Crc16Ccitt(crc_src, 8);
        vg93.sector_buffer[4] = (BYTE)(id_crc >> 8);
        vg93.sector_buffer[5] = (BYTE)(id_crc & 0xFF);

        vg93.transfer_pos = 0;
        vg93.transfer_len = 6;
        vg93.transfer_type = TRANSFER_READ_ADDRESS;
        vg93.drq = true;
        return;
    }

    if ((val & 0xF0) == 0xF0) {
        vg93.last_cmd_type1 = false;
        vg93.multi_sector = false;
        vg93.record_not_found = false;
        if (!VG93_DiskReady()) { VG93_EndCommand(true); return; }
        vg93.busy = true;
        vg93.intrq = false;
        vg93.transfer_type = TRANSFER_WRITE_TRACK;
        vg93.transfer_pos = 0;
        vg93.transfer_len = WRITE_TRACK_BYTES;
        vg93.drq = true;
        return;
    }

    if ((val & 0xF0) == 0xE0) {
        vg93.last_cmd_type1 = false;
        VG93_EndCommand(!VG93_DiskReady());
        return;
    }

    vg93.last_cmd_type1 = false;
    vg93.transfer_pos = -1;
    vg93.transfer_len = 256;
    vg93.multi_sector = false;
    vg93.record_not_found = false;
}

void VG93_WriteTrack(BYTE val) {
    if (!vg93.busy) vg93.track_reg = val;
}

void VG93_WriteSector(BYTE val) {
    if (!vg93.busy) vg93.sector_reg = val;
}

void VG93_WriteData(BYTE val) {
    vg93.data_reg = val;
    vg93.drq_since = cpu_tstates_total;

    if (vg93.transfer_type == TRANSFER_WRITE_TRACK) {
        if (vg93.transfer_pos < 0 || vg93.transfer_pos >= vg93.transfer_len) return;
        ++vg93.transfer_pos;
        if (vg93.transfer_pos < vg93.transfer_len) { vg93.drq = true; return; }
        vg93.drq = false;
        const bool ok = VG93_FinishWriteTrack();
        VG93_EndCommand(!ok);
        return;
    }

    if (vg93.transfer_type == TRANSFER_WRITE_SECTOR) {
        if (vg93.transfer_pos >= 0 && vg93.transfer_pos < vg93.transfer_len) {
            vg93.sector_buffer[vg93.transfer_pos++] = vg93.data_reg;
            vg93.drq = (vg93.transfer_pos < vg93.transfer_len);

            if (vg93.transfer_pos >= vg93.transfer_len) {
                if (VG93_FlushCurrentSector()) {
                    if (vg93.multi_sector) {
                        vg93.sector_reg++;
                        if (vg93.sector_reg > 16) { VG93_EndCommand(true); return; }
                        memset(vg93.sector_buffer, 0, 256);
                        vg93.transfer_pos = 0;
                        vg93.drq = true;
                    } else {
                        VG93_EndCommand(false);
                    }
                } else {
                    VG93_EndCommand(true);
                }
            }
        }
    }
}

void VG93_WriteSystem(BYTE val) {
    vg93.system_reg = val;
    if ((val & 0x04) == 0) VG93_ResetChip();
}

// --- СИСТЕМНАЯ ШИНА ПАМЯТИ ---
uint8_t ReadByte(uint16_t addr) {
    int target = addr & 0xFFFF;
    if (cpu.PC >= 0x3D00 && cpu.PC <= 0x3FFF && current_rom_page == 1 && !trdos_active) {
        trdos_active = true;
    }
    if (cpu.PC >= 0x4000 && trdos_active) {
        trdos_active = false; 
    }

    if (target < 0x4000) {
        if (trdos_active) return trdos_rom[target]; 
        return spec_rom[current_rom_page][target];   
    }
    if (target < 0x8000)  return spec_ram[5][target - 0x4000]; 
    if (target < 0xC000)  return spec_ram[2][target - 0x8000]; 
    return spec_ram[current_ram_page][target - 0xC000];
}

void WriteByte(uint16_t addr, uint8_t val) {
    int target = addr & 0xFFFF;
    if (target < 0x4000) return; 
    if (target < 0x8000) { spec_ram[5][target - 0x4000] = val; return; }
    if (target < 0xC000) { spec_ram[2][target - 0x8000] = val; return; }
    spec_ram[current_ram_page][target - 0xC000] = val; 
}

uint8_t InPort(uint16_t port) {
    if (trdos_active) {
        BYTE low_port = port & 0xFF;
        if (low_port == 0x1F) return VG93_ReadStatus(cpu.PC);
        if (low_port == 0x3F) return VG93_ReadTrack(cpu.PC);
        if (low_port == 0x5F) return VG93_ReadSector(cpu.PC);
        if (low_port == 0x7F) return VG93_ReadData(cpu.PC);
        if (low_port == 0xFF) return VG93_ReadSystem(cpu.PC);
    }

    if ((port & 0xFF) == 0xFE) {
        BYTE result = 0x1F;
        for (int i = 0; i < 8; i++) {
            if (!(port & (1 << (8 + i)))) result &= spec_key_rows[i];
        }
        return result | 0xC0 | (spec_ear_signal ? 0x20 : 0x00);
    }

    // Чтение AY чипа
    if ((port & 0xC002) == 0xC000) {
        if (ay.selected_reg == 14 || ay.selected_reg == 15) return 0xFF;
        return ay.regs[ay.selected_reg & 0x0F];
    }

    return 0xFF;
}

void OutPort(uint16_t port, uint8_t val) {
    if (trdos_active) {
        BYTE low_port = port & 0xFF;
        if (low_port == 0x1F) { VG93_WriteCommand(val); return; }
        if (low_port == 0x3F) { VG93_WriteTrack(val); return; }
        if (low_port == 0x5F) { VG93_WriteSector(val); return; }
        if (low_port == 0x7F) { VG93_WriteData(val); return; }
        if (low_port == 0xFF) { VG93_WriteSystem(val); return; }
    }

    if ((port & 0x8002) == 0x0000) {
        if (!memory_lock_128k) {
            current_ram_page = val & 0x07;
            screen_shadow_page = (val & 0x08) != 0;
            current_rom_page = (val & 0x10) ? 1 : 0;
            if (val & 0x20) memory_lock_128k = true;
        }
        return;
    }

    // Запись в AY чип
    if ((port & 0x8002) == 0x8000) {
        if (port & 0x4000) {
            ay.selected_reg = val & 0x0F;
            QueueAudioEvent(EVENT_AY_REG_SELECT, val & 0x0F);
        }
        else {
            QueueAudioEvent(EVENT_AY_REG_WRITE, val);
        }
        return;
    }

    if ((port & 0xFF) == 0xFE) {
        spec_speaker_state = (val & 0x10) != 0;
        QueueAudioEvent(EVENT_BEEPER_WRITE, spec_speaker_state ? 1 : 0);
        spec_border_color = val & 0x07;
    }
}


bool HandleRomHook(void) {
    if (cpu.PC == 0x0556 && tap_is_loaded) {
        if (tap_current_pos + 2 <= current_tap_size) {
            WORD block_len = current_tap_data[tap_current_pos] | (current_tap_data[tap_current_pos + 1] << 8);
            if (tap_current_pos + 2 + block_len <= current_tap_size) {
                size_t block_start = tap_current_pos + 2;
                WORD data_len = block_len - 2;
                const BYTE* raw_data = &current_tap_data[block_start + 1];

                WORD target_addr = cpu.IX; 
                for (WORD i = 0; i < data_len; i++) {
                    WriteByte(target_addr + i, raw_data[i]);
                }
                tap_current_pos += 2 + block_len;

                cpu.F |= (0x01 | 0x40); 
                cpu.A = 0u;             

                WORD low = ReadByte(cpu.SP);  cpu.SP = (cpu.SP + 1) & 0xFFFF;
                WORD high = ReadByte(cpu.SP); cpu.SP = (cpu.SP + 1) & 0xFFFF;
                cpu.PC = (high << 8) | low;
                return true;
            }
        }
        tap_is_loaded = false;
    }
    return false;
}
int cycles_until_interrupt = 70000;

void Reset(void) {
    init();
    cycles_until_interrupt = 70000;
    current_ram_page = 0;
    current_rom_page = 0;
    screen_shadow_page = false;
    memory_lock_128k = false;
    trdos_active = false;
}

int SystemStepZ80(void) {
    if (HandleRomHook()) return 20;
    return step();
}

void RenderSpectrumScreenToBuffer(void) {
    // ИСПРАВЛЕННАЯ ПАЛИТРА: ПОМЕНЯНЫ МЕСТАМИ ТОЛЬКО КРАСНЫЙ И СИНИЙ БАЙТЫ
    static const DWORD spec_palette[16] = {
        // --- ОБЫЧНЫЕ ЦВЕТА (BRIGHT 0) ---
        0x00000000, // 0: Черный
        0x00C00000, // 1: Синий
        0x000000C0, // 2: Красный
        0x00C000C0, // 3: Пурпурный
        0x0000C000, // 4: Зеленый
        0x00C0C000, // 5: Циан / Голубой
        0x0000C0C0, // 6: Желтый
        0x00C0C0C0, // 7: Белый / Серый

        // --- ЯРКИЕ ЦВЕТА (BRIGHT 1) ---
        0x00000000, // 8: Черный
        0x00FF0000, // 9: Синий яркий
        0x000000FF, // 10: Красный яркий
        0x00FF00FF, // 11: Пурпурный яркий
        0x0000FF00, // 12: Зеленый яркий
        0x00FFFF00, // 13: Циан яркий
        0x0000FFFF, // 14: Желтый яркий
        0x00FFFFFF  // 15: Чистый белый
    };

    int video_page = screen_shadow_page ? 7 : 5;
    int border_color_idx = spec_border_color & 0x07;
    DWORD border_color = spec_palette[border_color_idx];

    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        pixel_buffer[i] = border_color;
    }

    for (int y = 0; y < 192; y++) {
        int zone = (y >> 6) & 3;       
        int line = y & 7;              
        int char_row = (y >> 3) & 7;    
        WORD pixel_line_addr = 0x4000 + (zone << 11) + (line << 8) + (char_row << 5);
        int target_y = y + BORDER_HEIGHT;

        for (int x_byte = 0; x_byte < 32; x_byte++) {
            BYTE pixel_data = spec_ram[video_page][pixel_line_addr - 0x4000 + x_byte];
            int attr_addr = 0x5800 + ((y >> 3) * 32) + x_byte;
            BYTE attr = spec_ram[video_page][attr_addr - 0x4000];

            BYTE ink = attr & 0x07;
            BYTE paper = (attr >> 3) & 0x07;
            bool bright = (attr & 0x40) != 0;
            bool flash = (attr & 0x80) != 0;

            if (flash && spec_flash_state) {
                BYTE t = ink; ink = paper; paper = t;
            }

            int color_offset = bright ? 8 : 0;
            DWORD ink_color = spec_palette[ink + color_offset];
            DWORD paper_color = spec_palette[paper + color_offset];

            for (int bit = 0; bit < 8; bit++) {
                bool pixel_active = (pixel_data & (1 << (7 - bit))) != 0;
                int pixel_x = BORDER_WIDTH + (x_byte * 8) + bit;
                pixel_buffer[target_y * SCREEN_WIDTH + pixel_x] = pixel_active ? ink_color : paper_color;
            }
        }
    }
}

const uint32_t* zx128_get_pixel_buffer(void) {
    return pixel_buffer;
}

WORD ReadLE16(const BYTE* p) {
    return (WORD)p[0] | ((WORD)p[1] << 8);
}

bool LoadSnaSnapshot(const BYTE* data, size_t size) {
    if (size != 49179 && size != 131103) return false;
    Reset();
    spec_speaker_state = false;
    AY_Reset();

    cpu.I = data[0];
    cpu.L_alt = data[1]; cpu.H_alt = data[2];
    cpu.E_alt = data[3]; cpu.D_alt = data[4];
    cpu.C_alt = data[5]; cpu.B_alt = data[6];
    cpu.F_alt = data[7]; cpu.A_alt = data[8];
    cpu.L = data[9]; cpu.H = data[10];
    cpu.E = data[11]; cpu.D = data[12];
    cpu.C = data[13]; cpu.B = data[14];
    cpu.IY = ReadLE16(data + 15);
    cpu.IX = ReadLE16(data + 17);
    cpu.IFF1 = (data[19] & 0x04) != 0;
    cpu.IFF2 = cpu.IFF1;
    cpu.RR = data[20];
    cpu.F = data[21];
    cpu.A = data[22];
    cpu.SP = ReadLE16(data + 23);
    cpu.IM = data[25] & 0x03;
    if (cpu.IM > 2) cpu.IM = 2;
    spec_border_color = data[26] & 0x07;

    if (size == 49179) {
        current_rom_page = 1;      
        current_ram_page = 0;      
        screen_shadow_page = false;
        memory_lock_128k = true;   
        for (size_t i = 0; i < 16384; ++i) spec_ram[5][i] = data[27 + i];
        for (size_t i = 0; i < 16384; ++i) spec_ram[2][i] = data[27 + 16384 + i];
        for (size_t i = 0; i < 16384; ++i) spec_ram[0][i] = data[27 + 32768 + i];
        if (cpu.SP < 0x4000) return false;
        cpu.PC = (WORD)ReadByte(cpu.SP) | ((WORD)ReadByte(cpu.SP + 1) << 8);
        cpu.SP = (WORD)(cpu.SP + 2);
    }
    else {
        size_t ext_offset = 49179;
        cpu.PC = ReadLE16(data + ext_offset); 
        BYTE port_7ffd_val = data[ext_offset + 2];
        bool trdos_flag = data[ext_offset + 3] != 0;

        current_ram_page = port_7ffd_val & 0x07;
        screen_shadow_page = (port_7ffd_val & 0x08) != 0;
        current_rom_page = (port_7ffd_val & 0x10) ? 1 : 0;
        memory_lock_128k = (port_7ffd_val & 0x20) != 0;
        trdos_active = trdos_flag;

        for (size_t i = 0; i < 16384; ++i) spec_ram[5][i] = data[27 + i];          
        for (size_t i = 0; i < 16384; ++i) spec_ram[2][i] = data[27 + 16384 + i];  
        for (size_t i = 0; i < 16384; ++i) spec_ram[current_ram_page][i] = data[27 + 32768 + i]; 

        size_t page_data_ptr = ext_offset + 4;
        for (int p = 0; p < 8; p++) {
            if (p != 5 && p != 2 && p != current_ram_page) {
                if (page_data_ptr + 16384 <= size) {
                    for (size_t i = 0; i < 16384; ++i) spec_ram[p][i] = data[page_data_ptr + i];
                }
            }
            page_data_ptr += 16384; 
        }
    }
    cpu.halted = false;
    cpu.int_pending = false;
    cycles_until_interrupt = CYCLES_PER_STEP;
    return true;
}

bool zx128_load_sna(const char *filepath) {
    FILE *f = fopen(filepath, "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); size_t size = ftell(f); fseek(f, 0, SEEK_SET);
    BYTE* buf = malloc(size); if (!buf) { fclose(f); return false; }
    size_t r = fread(buf, 1, size, f); fclose(f);
    bool ok = LoadSnaSnapshot(buf, r); free(buf); return ok;
}

static bool DecompressZ80(const BYTE* src, size_t src_size,
                          BYTE* out, size_t expected_size,
                          bool old_format) {
    size_t p = 0; size_t out_idx = 0;
    while (p < src_size && out_idx < expected_size) {
        if (old_format && p + 4 <= src_size && src[p] == 0x00 && src[p + 1] == 0xED && src[p + 2] == 0xED && src[p + 3] == 0x00) break;
        if (p + 4 <= src_size && src[p] == 0xED && src[p + 1] == 0xED) {
            BYTE count = src[p + 2]; BYTE value = src[p + 3];
            if (count == 0 || out_idx + count > expected_size) return false;
            for (int k = 0; k < count; k++) out[out_idx++] = value;
            p += 4;
        } else {
            out[out_idx++] = src[p++];
        }
    }
    return out_idx == expected_size;
}

bool LoadZ80Snapshot(const BYTE* data, size_t size) {
    if (size < 30) return false;
    Reset();
    spec_speaker_state = false;
    AY_Reset();

    cpu.A = data[0]; cpu.F = data[1];
    cpu.B = (BYTE)(ReadLE16(data + 2) >> 8); cpu.C = (BYTE)(ReadLE16(data + 2) & 0xFF); 
    cpu.H = (BYTE)(ReadLE16(data + 4) >> 8); cpu.L = (BYTE)(ReadLE16(data + 4) & 0xFF); 
    WORD pc_header = ReadLE16(data + 6);
    cpu.SP = ReadLE16(data + 8);
    cpu.I = data[10];
    cpu.RR = (BYTE)((data[11] & 0x7F) | ((data[12] & 0x01) << 7));
    spec_border_color = (data[12] >> 1) & 0x07;
    cpu.D = (BYTE)(ReadLE16(data + 13) >> 8); cpu.E = (BYTE)(ReadLE16(data + 13) & 0xFF); 
    cpu.C_alt = data[15]; cpu.B_alt = data[16];
    cpu.E_alt = data[17]; cpu.D_alt = data[18];
    cpu.L_alt = data[19]; cpu.H_alt = data[20];
    cpu.A_alt = data[21]; cpu.F_alt = data[22];
    cpu.IY = ReadLE16(data + 23);
    cpu.IX = ReadLE16(data + 25);
    cpu.IFF1 = data[27] != 0;
    cpu.IFF2 = data[28] != 0;
    cpu.IM = data[29] & 0x03;
    if (cpu.IM > 2) cpu.IM = 2;

    if (pc_header != 0) {
        current_rom_page = 1; current_ram_page = 0; screen_shadow_page = false; memory_lock_128k = true; trdos_active = false;
        const bool compressed = (data[12] & 0x20) != 0;
        BYTE* ram = malloc(49152); if (!ram) return false;
        if (compressed) {
            if (!DecompressZ80(data + 30, size - 30, ram, 49152, true)) { free(ram); return false; }
        } else {
            if (size < 30 + 49152) { free(ram); return false; }
            memcpy(ram, data + 30, 49152);
        }
        for (size_t i = 0; i < 16384; ++i) spec_ram[5][i] = ram[i];
        for (size_t i = 0; i < 16384; ++i) spec_ram[2][i] = ram[16384 + i];
        for (size_t i = 0; i < 16384; ++i) spec_ram[0][i] = ram[32768 + i];
        cpu.PC = pc_header;
        free(ram);
    }
    else {
        if (size < 32) return false;
        WORD ext_len = ReadLE16(data + 30);
        if (ext_len < 23 || 32u + ext_len > size) return false;
        const BYTE* ext = data + 32;
        cpu.PC = ReadLE16(ext); 

        BYTE hardware_mode = ext[2];
        BYTE port_7ffd_val = ext[5];

        current_ram_page = port_7ffd_val & 0x07;
        screen_shadow_page = (port_7ffd_val & 0x08) != 0;
        current_rom_page = (port_7ffd_val & 0x10) ? 1 : 0;
        memory_lock_128k = (port_7ffd_val & 0x20) != 0;

        if (ext_len >= 54) {
            if (hardware_mode == 12 || hardware_mode == 13 || (ext[48] & 0x01)) trdos_active = true;
        }

        for (int p = 0; p < 8; p++) memset(spec_ram[p], 0, 16384);

        size_t pos = 32u + ext_len;
        BYTE* block = malloc(16384); if (!block) return false;
        while (pos + 3 <= size) {
            WORD block_len = ReadLE16(data + pos); BYTE page_id = data[pos + 2]; pos += 3;
            size_t stored_size = (block_len == 0xFFFF) ? 16384u : (size_t)block_len;
            if (pos + stored_size > size) { free(block); return false; }

            int target_page_index = -1;
            if (page_id >= 3 && page_id <= 10) target_page_index = page_id - 3;
            else if (page_id == 0) target_page_index = 5;

            if (target_page_index >= 0 && target_page_index < 8) {
                if (block_len == 0xFFFF) {
                    memcpy(spec_ram[target_page_index], data + pos, 16384);
                } else {
                    if (!DecompressZ80(data + pos, block_len, block, 16384, false)) { free(block); return false; }
                    memcpy(spec_ram[target_page_index], block, 16384);
                }
            }
            pos += stored_size;
        }
        free(block);
    }
    cpu.halted = false; cpu.int_pending = false; cycles_until_interrupt = CYCLES_PER_STEP;
    return true;
}

bool zx128_load_z80(const char *filepath) {
    FILE *f = fopen(filepath, "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); size_t size = ftell(f); fseek(f, 0, SEEK_SET);
    BYTE* buf = malloc(size); if (!buf) { fclose(f); return false; }
    size_t r = fread(buf, 1, size, f); fclose(f);
    bool ok = LoadZ80Snapshot(buf, r); free(buf); return ok;
}

bool LoadTapFile(const BYTE* data, size_t size) {
    if (size == 0) return false;
    if (current_tap_data) free(current_tap_data);
    current_tap_data = malloc(size);
    if (!current_tap_data) return false;
    memcpy(current_tap_data, data, size);
    current_tap_size = size;
    tap_current_pos = 0;
    tap_is_loaded = true;

    Reset();
    for (int p = 0; p < 8; p++) memset(spec_ram[p], 0, 16384);
    AY_Reset();

    current_ram_page = 0; current_rom_page = 0; screen_shadow_page = false; memory_lock_128k = false; trdos_active = false;
    for (int i = 0; i < 8; i++) { spec_key_rows[i] = 0x1F; physical_key_rows[i] = 0x1F; gui_key_rows[i] = 0x1F; }
    return true;
}

bool zx128_load_tape(const char *filepath) {
    FILE *f = fopen(filepath, "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); size_t size = ftell(f); fseek(f, 0, SEEK_SET);
    BYTE* buf = malloc(size); if (!buf) { fclose(f); return false; }
    size_t r = fread(buf, 1, size, f); fclose(f);
    bool ok = LoadTapFile(buf, r); free(buf); return ok;
}

bool LoadTrdFile(const BYTE* data, size_t size) {
    if (size == 0) return false;
    if (current_trd_data) free(current_trd_data);
    
    current_trd_size = (size < 655360) ? 655360 : size;
    current_trd_data = malloc(current_trd_size);
    if (!current_trd_data) return false;
    
    memset(current_trd_data, 0, current_trd_size);
    memcpy(current_trd_data, data, size);

    if (current_trd_size >= 655360) {
        size_t sys_offset = 2048; 
        current_trd_data[sys_offset + 225] = 0x16; 
        current_trd_data[sys_offset + 226] = 0x01;
    }

    trd_is_inserted = true;
    VG93_Reset();
    Reset();
    for (int p = 0; p < 8; p++) memset(spec_ram[p], 0, 16384);
    AY_Reset();

    for (int i = 0; i < 8; i++) { spec_key_rows[i] = 0x1F; physical_key_rows[i] = 0x1F; gui_key_rows[i] = 0x1F; }
    current_ram_page = 0; current_rom_page = 0; screen_shadow_page = false; memory_lock_128k = false; trdos_active = false;
    return true;
}

bool zx128_load_trd(const char *filepath) {
    FILE *f = fopen(filepath, "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); size_t size = ftell(f); fseek(f, 0, SEEK_SET);
    BYTE* buf = malloc(size); if (!buf) { fclose(f); return false; }
    size_t r = fread(buf, 1, size, f); fclose(f);
    bool ok = LoadTrdFile(buf, r); free(buf); return ok;
}

bool LoadSclFile(const BYTE* data, size_t size) {
    if (size < 13) return false;
    if (data[0] != 'S' || data[1] != 'I' || data[2] != 'N' || data[3] != 'C' || data[4] != 'L' || data[5] != 'A' || data[6] != 'I' || data[7] != 'R') return false;

    const BYTE num_files = data[8];
    size_t expected_headers_size = 9 + (size_t)num_files * 14;
    if (size < expected_headers_size + 4) return false;

    size_t scl_data_end = size - 4;
    size_t TRD_SIZE = 655360;
    BYTE* trd_image = malloc(TRD_SIZE); if (!trd_image) return false;
    memset(trd_image, 0, TRD_SIZE);

    size_t sys_sector_offset = 8 * 256;
    trd_image[sys_sector_offset + 227] = 0x16; 
    trd_image[sys_sector_offset + 231] = 0x10; 
    memcpy(&trd_image[sys_sector_offset + 245], "SCL_DISK", 8);

    BYTE current_sector = 0;
    BYTE current_track = 1;
    BYTE total_files_copied = 0;
    size_t scl_file_data_ptr = expected_headers_size;

    for (BYTE f = 0; f < num_files; ++f) {
        size_t scl_header_ptr = 9 + (size_t)f * 14;
        const BYTE file_type = data[scl_header_ptr + 8];
        const WORD file_param1 = ReadLE16(data + scl_header_ptr + 9);
        const WORD file_param2 = ReadLE16(data + scl_header_ptr + 11);
        const BYTE file_sectors = data[scl_header_ptr + 13];
        const size_t file_storage_size = (size_t)file_sectors * 256;

        if (scl_file_data_ptr > scl_data_end || file_storage_size > scl_data_end - scl_file_data_ptr) { free(trd_image); return false; }
        size_t used_sectors = (size_t)(current_track - 1) * 16 + current_sector;
        if (used_sectors > 2544 || (size_t)file_sectors > 2544 - used_sectors) { free(trd_image); return false; }
        if (total_files_copied >= 128) { free(trd_image); return false; }

        size_t trd_catalog_ptr = (size_t)total_files_copied * 16;
        memcpy(&trd_image[trd_catalog_ptr], &data[scl_header_ptr], 8);
        trd_image[trd_catalog_ptr + 8] = file_type;
        trd_image[trd_catalog_ptr + 9] = (BYTE)(file_param1 & 0xFF);
        trd_image[trd_catalog_ptr + 10] = (BYTE)((file_param1 >> 8) & 0xFF);
        trd_image[trd_catalog_ptr + 11] = (BYTE)(file_param2 & 0xFF);
        trd_image[trd_catalog_ptr + 12] = (BYTE)((file_param2 >> 8) & 0xFF);
        trd_image[trd_catalog_ptr + 13] = file_sectors;
        trd_image[trd_catalog_ptr + 14] = current_sector;
        trd_image[trd_catalog_ptr + 15] = current_track;

        size_t trd_data_offset = (size_t)(current_track * 16 + current_sector) * 256;
        if (trd_data_offset > TRD_SIZE || file_storage_size > TRD_SIZE - trd_data_offset) { free(trd_image); return false; }
        if (file_storage_size != 0) memcpy(&trd_image[trd_data_offset], &data[scl_file_data_ptr], file_storage_size);

        ++total_files_copied;
        size_t next_sector = (size_t)current_sector + file_sectors;
        while (next_sector >= 16) { next_sector -= 16; ++current_track; }
        current_sector = (BYTE)next_sector;
        scl_file_data_ptr += file_storage_size;
    }

    trd_image[sys_sector_offset + 225] = current_sector;
    trd_image[sys_sector_offset + 226] = current_track;
    trd_image[sys_sector_offset + 227] = 0x16;
    trd_image[sys_sector_offset + 228] = total_files_copied;

    size_t used_data_sectors = (size_t)(current_track - 1) * 16 + current_sector;
    WORD free_sectors = (WORD)(2544 - used_data_sectors);
    trd_image[sys_sector_offset + 229] = (BYTE)(free_sectors & 0xFF);
    trd_image[sys_sector_offset + 230] = (BYTE)((free_sectors >> 8) & 0xFF);
    trd_image[sys_sector_offset + 231] = 0x10;

    bool ok = LoadTrdFile(trd_image, TRD_SIZE);
    free(trd_image);
    return ok;
}

bool zx128_load_scl(const char *filepath) {
    FILE *f = fopen(filepath, "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); size_t size = ftell(f); fseek(f, 0, SEEK_SET);
    BYTE* buf = malloc(size); if (!buf) { fclose(f); return false; }
    size_t r = fread(buf, 1, size, f); fclose(f);
    bool ok = LoadSclFile(buf, r); free(buf); return ok;
}

// --- СИНХРОНИЗИРОВАННЫЙ МАППИНГ КЛАВИАТУРЫ ---
typedef struct {
    int row; // Индекс полуряда 0..7
    int bit; // Бит 0..4
} KeyMapInfo;

// Абсолютное соответствие порядку: 1..0, Q..P, A..ENTER, C_SHIFT..SPACE
static const KeyMapInfo spec_hardware_map[40] = {
    {3,0}, {3,1}, {3,2}, {3,3}, {3,4}, {4,4}, {4,3}, {4,2}, {4,1}, {4,0}, // 1 2 3 4 5 6 7 8 9 0
    {2,0}, {2,1}, {2,2}, {2,3}, {2,4}, {5,4}, {5,3}, {5,2}, {5,1}, {5,0}, // Q W E R T Y U I O P
    {1,0}, {1,1}, {1,2}, {1,3}, {1,4}, {6,4}, {6,3}, {6,2}, {6,1}, {6,0}, // A S D F G H J K L ENTER
    {0,0}, {0,1}, {0,2}, {0,3}, {0,4}, {7,4}, {7,3}, {7,2}, {7,1}, {7,0}  // C_SHIFT Z X C V B N M S_SHIFT SPACE
};

void RebuildSpectrumKeyboardMatrix(void) {
    for (int i = 0; i < 8; ++i) {
        spec_key_rows[i] = (BYTE)(physical_key_rows[i] & gui_key_rows[i]);
    }
}

void SetPhysicalMatrixKey(int idx, bool down) {
    if (idx < 0 || idx >= 40) return;
    int r = spec_hardware_map[idx].row;
    int b = spec_hardware_map[idx].bit;
    if (down) physical_key_rows[r] &= (BYTE)~(1u << b);
    else physical_key_rows[r] |= (BYTE)(1u << b);
    RebuildSpectrumKeyboardMatrix();
}

void PulseGuiMatrixKey(int idx) {
    if (idx < 0 || idx >= 40) return;
    int r = spec_hardware_map[idx].row;
    int b = spec_hardware_map[idx].bit;
    gui_key_rows[r] &= (BYTE)~(1u << b);
    gui_key_frames[idx] = 10;
    RebuildSpectrumKeyboardMatrix();
}

void zx128_init(const char *rom1, const char *rom2, const char *trdos) {
    memset(spec_rom, 0, sizeof(spec_rom));
    memset(spec_ram, 0, sizeof(spec_ram));

    FILE *f1 = fopen(rom1, "rb");
    if (f1) {
        fread(spec_rom[0], 1, 16384, f1);
        fclose(f1);
    }
    FILE *f2 = fopen(rom2, "rb");
    if (f2) {
        fread(spec_rom[1], 1, 16384, f2);
        fclose(f2);
    }
    FILE *ftr = fopen(trdos, "rb");
    if (ftr) { fread(trdos_rom, 1, 16384, ftr); fclose(ftr); }

    ay.stereo_mode = MODE_ABC;
    AY_Reset();
    init();
    for (int i = 0; i < 8; i++) { spec_key_rows[i] = 0x1F; physical_key_rows[i] = 0x1F; gui_key_rows[i] = 0x1F; }
}

void zx128_run_frame(void) {
    int cycles_executed = 0;
    // На 1 кадр Spectrum 128 приходится ровно 70908 тактов
    while (cycles_executed < 70908) {
        int cycles = SystemStepZ80();
        cycles_executed += cycles;
        cpu_tstates_total += (uint32_t)cycles;

        zx128_tick_audio((uint32_t)cycles);
    }
    z80_interrupt_now(0xFF);
    if (++spec_flash_frame >= 16) {
        spec_flash_frame = 0;
        spec_flash_state = !spec_flash_state;
    }
    
    for (int k = 0; k < 40; ++k) {
        if (gui_key_frames[k] > 0) {
            --gui_key_frames[k];
            if (gui_key_frames[k] == 0) {
                int r = spec_hardware_map[k].row;
                int b = spec_hardware_map[k].bit;
                gui_key_rows[r] |= (BYTE)(1u << b);
            }
        }
    }
    RebuildSpectrumKeyboardMatrix();
    RenderSpectrumScreenToBuffer();
}

void zx128_reset(void) {
    ClearAudioBuffers();
    Reset();
    
    cpu_tstates_total = 0;
    audio_cursor_tstates = 0;
    ay_clock_phase = 0;
    audio_sample_phase = 0;
    noise_prescaler = 0;
    envelope_prescaler = 0;
    
    spec_speaker_state = false;
    AY_Reset();
    
    for (int i = 0; i < 8; i++) memset(spec_ram[i], 0, 16384);
    for (int i = 0; i < 8; i++) { spec_key_rows[i] = 0x1F; physical_key_rows[i] = 0x1F; gui_key_rows[i] = 0x1F; }
    for (int i = 0; i < 40; ++i) gui_key_frames[i] = 0;
    spec_flash_frame = 0;
    spec_flash_state = false;
}




