//
//  CPUZ80_MACOS.h
//  simulator_zx48_mac
//
//  Адаптировано под macOS (Apple Silicon / Intel)
//

#ifndef CPUZ80_MACOS_H
#define CPUZ80_MACOS_H

#include <stdint.h>
#include <stdbool.h>

#define Z80_CPU_SPEED 3500000 /* В Гц: 3.5 МГц тактовая частота ZX Spectrum */
#define CYCLES_PER_FRAME (Z80_CPU_SPEED / 50) /* 70000 тактов на один кадр 50 Гц */

#ifdef __cplusplus
extern "C" {
#endif

// --- ПРОТОТИПЫ ПЛОСКИХ СИ-ФУНКЦИЙ ИЗ ОРИГИНАЛЬНОГО ЯДРА Z80.C ---
void init(void);
int step(void);
void gen_nmi(void);
void gen_int(uint8_t data);
int z80_interrupt_now(uint8_t data);

// --- СТРУКТУРА ПРОЦЕССОРА Z80 ---
// В точности повторяет бинарное расположение данных из z80.c,
// но использует инлайны вместо __thiscall методов для совместимости с macOS Clang.
struct CPUZ80 {
    uint16_t PC, SP, IX, IY;
    uint16_t wz;
    uint8_t A, B, C, D, E, H, L;
    uint8_t A_alt, B_alt, C_alt, D_alt, E_alt, H_alt, L_alt, F_alt;
    uint8_t I, RR;
    uint8_t F;
    uint8_t iff_set;
    bool IFF1, IFF2;
    uint8_t IM;
    uint8_t int_vec;
    bool int_pending, nmi_pending;
    bool halted;

#ifdef __cplusplus
    // Обёртки для C++ / Objective-C++ кода, чтобы сохранить привычный синтаксис `cpu.step()`
    inline void init() { ::init(); }
    inline int step() { return ::step(); }
    inline void gen_nmi() { ::gen_nmi(); }
    inline void gen_int(uint8_t data) { ::gen_int(data); }
    inline int z80_interrupt_now(uint8_t data) { return ::z80_interrupt_now(data); }
#endif
};

// Объявление глобального объекта процессора, который реально выделен в z80.c
extern struct CPUZ80 cpu;

#ifdef __cplusplus
} // extern "C"
#endif

// --- ХЕЛПЕРЫ ДЛЯ РАБОТЫ С РЕГИСТРОВЫМИ ПАРАМИ ---
// Оформлены как статические inline-функции для предотвращения ошибок линковки (Multiple Definitions)
static inline uint16_t GetHL(void) { return ((uint16_t)cpu.H << 8) | cpu.L; }
static inline void SetHL(uint16_t val) { cpu.H = (uint8_t)(val >> 8); cpu.L = (uint8_t)(val & 0xFF); }

static inline uint16_t GetBC(void) { return ((uint16_t)cpu.B << 8) | cpu.C; }
static inline void SetBC(uint16_t val) { cpu.B = (uint8_t)(val >> 8); cpu.C = (uint8_t)(val & 0xFF); }

static inline uint16_t GetDE(void) { return ((uint16_t)cpu.D << 8) | cpu.E; }
static inline void SetDE(uint16_t val) { cpu.D = (uint8_t)(val >> 8); cpu.E = (uint8_t)(val & 0xFF); }

static inline uint8_t get_f(void) { return cpu.F; }
static inline void set_f(uint8_t val) { cpu.F = val; }

#endif /* CPUZ80_MACOS_H */
