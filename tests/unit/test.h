/*
 * Salsa64 — marco de pruebas unitarias del núcleo.
 *
 * Las mismas pruebas corren en el host (gcc nativo, little-endian:
 * `make test-host`) y en la PS3/RPCS3 (ppu-gcc, big-endian:
 * tests/phase1/cpu-tests). Sin dependencias de la plataforma: la salida va
 * por la función que pasa quien llama a salsa64_unit_run.
 *
 * Cada prueba arranca con una máquina limpia (t_reset): RDRAM en cero,
 * modo kernel, Status = CU1|CU0|FR, PC en T_CODE. Las pruebas ponen los
 * registros a mano, cargan unas pocas instrucciones con T_LOAD y avanzan con
 * t_step.
 */
#ifndef SALSA64_TESTS_TEST_H
#define SALSA64_TESTS_TEST_H

#include <stdint.h>

#include "core/n64.h"
#include "mips.h"

#define T_CODE_PA   0x1000u
#define T_DATA_PA   0x2000u
#define T_CODE      0xFFFFFFFF80001000ull  /* KSEG0 */
#define T_DATA      0xFFFFFFFF80002000ull
#define T_VEC_GEN   0xFFFFFFFF80000180ull
#define T_VEC_TLB   0xFFFFFFFF80000000ull
#define T_STATUS    (SALSA64_ST_CU0 | SALSA64_ST_CU1 | SALSA64_ST_FR)

extern struct salsa64_n64 t_n64;
#define CPU (&t_n64.cpu)
#define R   (t_n64.cpu.gpr)

extern int t_checks;

void t_fail(const char *file, int line, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#define CHECK(c)                                              \
	do {                                                  \
		t_checks++;                                   \
		if (!(c))                                     \
			t_fail(__FILE__, __LINE__, "%s", #c); \
	} while (0)

#define CHECK_EQ(a, b)                                                                 \
	do {                                                                           \
		uint64_t a_ = (uint64_t)(a), b_ = (uint64_t)(b);                       \
		t_checks++;                                                            \
		if (a_ != b_)                                                          \
			t_fail(__FILE__, __LINE__, "%s = 0x%016llx, se esperaba 0x%016llx", \
			       #a, (unsigned long long)a_, (unsigned long long)b_);    \
	} while (0)

/* Excepción tomada: código, EPC, BD y vector. */
#define CHECK_EXC(x_code, x_epc, x_bd, x_vec)                             \
	do {                                                              \
		CHECK_EQ(SALSA64_CAUSE_EXC(CPU->cause), x_code);          \
		CHECK_EQ(CPU->epc, x_epc);                                \
		CHECK_EQ((CPU->cause & SALSA64_CAUSE_BD) != 0, x_bd);     \
		CHECK_EQ(CPU->pc, x_vec);                                 \
		CHECK(CPU->status & SALSA64_ST_EXL);                      \
	} while (0)

void t_load(const uint32_t *code, unsigned n);
#define T_LOAD(...)                                                 \
	do {                                                        \
		static const uint32_t code_[] = { __VA_ARGS__ };    \
		t_load(code_, sizeof code_ / sizeof code_[0]);      \
	} while (0)

void t_step(unsigned n);
void t_set_status(uint32_t status);

/* Escribe una entrada de la TLB ejecutando TLBWI desde una dirección aparte
 * (0x80000F00), sin tocar el programa de la prueba. Deja Index, EntryHi,
 * EntryLo0/1 y PageMask con esos valores. */
void t_tlb_set(unsigned index, uint64_t hi, uint32_t lo0, uint32_t lo1, uint32_t page_mask);
/* EntryLo: PFN | C=3 | D | V | G */
#define T_LO(pfn, d, v, g) ((uint32_t)(pfn) << 6 | 3u << 3 | (d) << 2 | (v) << 1 | (g))

/* RDRAM física, en el orden de bytes de la N64. */
uint32_t t_rd32(uint32_t pa);
uint64_t t_rd64(uint32_t pa);
void t_wr32(uint32_t pa, uint32_t v);
void t_wr64(uint32_t pa, uint64_t v);

/* Patrones de bits de float/double del host (IEEE 754 en ambos). */
uint32_t t_fbits(float f);
uint64_t t_dbits(double d);

typedef void (*t_test_fn)(void);
void t_run(const char *name, t_test_fn fn);
#define RUN(fn) t_run(#fn, fn)

/* Suites (una por archivo). */
void t_suite_core(void);
void t_suite_mempak(void);
void t_suite_faultlog(void);
void t_suite_alu(void);
void t_suite_muldiv(void);
void t_suite_branch(void);
void t_suite_mem(void);
void t_suite_cop0(void);
void t_suite_cop1(void);
void t_suite_boot(void);
void t_suite_vi(void);
void t_suite_gamefix(void);
void t_suite_ai(void);
void t_suite_pif(void);
void t_suite_rompg(void);
void t_suite_archiverom(void);
void t_suite_transferpak(void);
void t_suite_idle(void);
void t_suite_save(void);
void t_suite_flashram(void);
void t_suite_rsp(void);
void t_suite_rsp_lle(void);
void t_suite_gfx(void);
void t_suite_gfx_f3d(void);
void t_suite_gfx_rare(void);
void t_suite_gfx_acclaim(void);
void t_suite_gfx_f5(void);
void t_suite_gfx_f5indy(void);
void t_suite_gfx_turbo3d(void);
void t_suite_gfx_selftest(void);
void t_suite_jpeg(void);
void t_suite_re2(void);
void t_suite_audio(void);
void t_suite_audio_sync(void);
void t_suite_pacer(void);
void t_suite_targets(void);
void t_suite_vmatch(void);
void t_suite_raster(void);
void t_suite_gfxcheck(void);
void t_suite_fpgen(void);
void t_suite_sifm(void);
void t_suite_menu(void);
void t_suite_crt(void);
void t_suite_seda(void);
void t_suite_cartucho(void);
void t_suite_state(void);
void t_suite_states_frontend(void);
void t_suite_lz(void);
void t_suite_gfxdump(void);
void t_suite_blackbox(void);
void t_suite_osd(void);
void t_suite_prompts(void);
void t_suite_pausectl(void);
void t_suite_rec_profile(void);
void t_suite_rec_sample(void);
void t_suite_rec_trace(void);
void t_suite_rec_copy(void);
void t_suite_perfilado(void);
void t_suite_cheats(void);
void t_suite_cover(void);
void t_suite_intro(void);
void t_suite_dlutil(void);
void t_suite_ra(void);
void t_suite_i18n(void);
void t_suite_cuelgue(void);
void t_suite_scale_auto(void);

/* Directorio donde las pruebas pueden escribir (lo pone quien llama; NULL:
 * las pruebas que tocan el disco no corren). */
extern const char *t_tmp_dir;

/* Corre todo y devuelve el número de pruebas fallidas. `print` recibe una
 * línea sin salto final. */
int salsa64_unit_run(void (*print)(const char *line));
/* Lo mismo con el recompilador (rec.h) sobre la caché `code`; -1 si el host
 * no puede ejecutar código generado. */
struct salsa64_rec;
/* Correr solo las pruebas cuyo nombre contiene `substring` (NULL: todas). */
void salsa64_unit_filter(const char *substring);
int salsa64_unit_run_rec(void (*print)(const char *line), struct salsa64_rec *rec, void *code, size_t bytes);

/* Micro-benchmark del intérprete (bench.c). Llamar después de
 * salsa64_unit_run, que inicializa la máquina. */
void salsa64_unit_bench(void (*print)(const char *line), uint64_t (*now_us)(void));
/* Diagnóstico: el bench del intérprete en una máquina alineada a 64 KB. */
void salsa64_unit_bench_aligned(void (*print)(const char *line), uint64_t (*now_us)(void));

#endif
