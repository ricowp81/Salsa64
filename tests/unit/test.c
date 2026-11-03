/*
 * Salsa64 — marco y arnés de las pruebas unitarias. Ver test.h.
 */
#include "test.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/endian.h"
#include "cpu/rec.h"

struct salsa64_n64 t_n64;
int t_checks;

static uint8_t t_rdram[SALSA64_RDRAM_MAX] __attribute__((aligned(128)));
static void (*t_print)(const char *line);
static const char *t_current;
static int t_current_failed;
static int t_tests, t_failed_tests;

void t_fail(const char *file, int line, const char *fmt, ...)
{
	char msg[512];
	int n = snprintf(msg, sizeof msg, "FALLO %s (%s:%d): ", t_current, file, line);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg + n, sizeof msg - (size_t)n, fmt, ap);
	va_end(ap);
	t_print(msg);
	t_current_failed = 1;
}

void t_set_status(uint32_t status)
{
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_STATUS, status);
}

static void t_reset(void)
{
	salsa64_n64_reset(&t_n64);
	t_set_status(T_STATUS);
	salsa64_cpu_set_pc(CPU, T_CODE);
}

void t_load(const uint32_t *code, unsigned n)
{
	for (unsigned k = 0; k < n; k++)
		t_wr32(T_CODE_PA + 4 * k, code[k]);
}

/* Con el recompilador, n instrucciones exactas por el despachador (modo
 * estricto: compila recortado lo que no cabe); salsa64_cpu_step es una
 * instrucción del intérprete seguida de los eventos que venzan, lo mismo. */
void t_step(unsigned n)
{
	if (CPU->rec) {
		salsa64_n64_run(&t_n64, n);
		return;
	}
	while (n--)
		salsa64_cpu_step(CPU);
}

void t_tlb_set(unsigned index, uint64_t hi, uint32_t lo0, uint32_t lo1, uint32_t page_mask)
{
	uint64_t pc = CPU->pc;
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_INDEX, index);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_ENTRYHI, hi);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_ENTRYLO0, lo0);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_ENTRYLO1, lo1);
	salsa64_cpu_cop0_write(CPU, SALSA64_CP0_PAGEMASK, page_mask);
	t_wr32(0x0F00, TLBWI);
	salsa64_cpu_set_pc(CPU, 0xFFFFFFFF80000F00ull);
	t_step(1);
	salsa64_cpu_set_pc(CPU, pc);
}

uint32_t t_rd32(uint32_t pa) { return salsa64_rd32(t_rdram + pa); }
uint64_t t_rd64(uint32_t pa) { return salsa64_rd64(t_rdram + pa); }

/* Las pruebas escriben la RDRAM por fuera del CPU: avisar al recompilador. */
void t_wr32(uint32_t pa, uint32_t v)
{
	salsa64_wr32(t_rdram + pa, v);
	salsa64_bus_rdram_written(&t_n64.bus, pa, 4);
}

void t_wr64(uint32_t pa, uint64_t v)
{
	salsa64_wr64(t_rdram + pa, v);
	salsa64_bus_rdram_written(&t_n64.bus, pa, 8);
}

uint32_t t_fbits(float f) { uint32_t u; memcpy(&u, &f, sizeof u); return u; }
uint64_t t_dbits(double d) { uint64_t u; memcpy(&u, &d, sizeof u); return u; }

/* Solo las pruebas cuyo nombre contenga este texto (salsa64_unit_filter). */
static const char *t_only;

void salsa64_unit_filter(const char *substring)
{
	t_only = substring;
}

void t_run(const char *name, t_test_fn fn)
{
	if (t_only && !strstr(name, t_only))
		return;
	t_current = name;
	t_current_failed = 0;
	t_reset();
	fn();
	t_tests++;
	t_failed_tests += t_current_failed;
}

static int run_suites(void (*print)(const char *line), struct salsa64_rec *rec, void *code, size_t bytes)
{
	static const struct {
		const char *name;
		void (*fn)(void);
	} suites[] = {
		{ "core", t_suite_core },
		{ "alu", t_suite_alu },
		{ "muldiv", t_suite_muldiv },
		{ "branch", t_suite_branch },
		{ "mem", t_suite_mem },
		{ "cop0", t_suite_cop0 },
		{ "cop1", t_suite_cop1 },
		{ "boot", t_suite_boot },
		{ "vi", t_suite_vi },
		{ "gamefix", t_suite_gamefix },
		{ "ai", t_suite_ai },
		{ "pif", t_suite_pif },
		{ "rompg", t_suite_rompg },
		{ "archiverom", t_suite_archiverom },
		{ "transferpak", t_suite_transferpak },
		{ "idle", t_suite_idle },
		{ "save", t_suite_save },
		{ "mempak", t_suite_mempak },
		{ "faultlog", t_suite_faultlog },
		{ "flashram", t_suite_flashram },
		{ "rsp", t_suite_rsp },
		{ "rsp-lle", t_suite_rsp_lle },
		{ "gfx", t_suite_gfx },
		{ "gfx-f3d", t_suite_gfx_f3d },
		{ "gfx-rare", t_suite_gfx_rare },
		{ "gfx-acclaim", t_suite_gfx_acclaim },
		{ "gfx-f5", t_suite_gfx_f5 },
		{ "gfx-f5indy", t_suite_gfx_f5indy },
		{ "gfx-turbo3d", t_suite_gfx_turbo3d },
		{ "gfx-selftest", t_suite_gfx_selftest },
		{ "jpeg", t_suite_jpeg },
		{ "re2", t_suite_re2 },
		{ "audio", t_suite_audio },
		{ "sync", t_suite_audio_sync },
		{ "pacer", t_suite_pacer },
		{ "targets", t_suite_targets },
		{ "vmatch", t_suite_vmatch },
		{ "raster", t_suite_raster },
		{ "gfxcheck", t_suite_gfxcheck },
		{ "fpgen", t_suite_fpgen },
		{ "sifm", t_suite_sifm },
		{ "menu", t_suite_menu },
		{ "crt", t_suite_crt },
		{ "seda", t_suite_seda },
		{ "cartucho", t_suite_cartucho },
		{ "state", t_suite_state },
		{ "states", t_suite_states_frontend },
		{ "lz", t_suite_lz },
		{ "gfxdump", t_suite_gfxdump },
		{ "blackbox", t_suite_blackbox },
		{ "osd", t_suite_osd },
		{ "prompts", t_suite_prompts },
		{ "pausectl", t_suite_pausectl },
		{ "perfil", t_suite_rec_profile },
		{ "muestreo", t_suite_rec_sample },
		{ "traza", t_suite_rec_trace },
		{ "copias", t_suite_rec_copy },
		{ "perfilado", t_suite_perfilado },
		{ "cheats", t_suite_cheats },
		{ "cover", t_suite_cover },
		{ "intro", t_suite_intro },
		{ "dlutil", t_suite_dlutil },
		{ "ra", t_suite_ra },
		{ "i18n", t_suite_i18n },
		{ "cuelgue", t_suite_cuelgue },
		{ "escala", t_suite_scale_auto },
	};
	char line[256];

	t_print = print;
	t_tests = t_failed_tests = t_checks = 0;
	salsa64_n64_init(&t_n64, t_rdram, sizeof t_rdram);
	if (rec) {
		if (!rec->hot_bytes)
			rec->hot_bytes = (uint32_t)(bytes / 16);  /* zona caliente para las pruebas del perfil */
		if (!salsa64_rec_init(rec, CPU, code, bytes)) {
			print("recompilador: no disponible en este host");
			return -1;
		}
		rec->strict = true;
		print("recompilador: activo (modo estricto)");
	}

	for (unsigned s = 0; s < sizeof suites / sizeof suites[0]; s++) {
		int tests0 = t_tests, failed0 = t_failed_tests;
		suites[s].fn();
		snprintf(line, sizeof line, "suite %-7s %3d pruebas, %d fallidas",
			 suites[s].name, t_tests - tests0, t_failed_tests - failed0);
		print(line);
	}
	snprintf(line, sizeof line, "total: %d pruebas, %d verificaciones, %d fallidas (%s%s)",
		 t_tests, t_checks, t_failed_tests,
		 SALSA64_HOST_BIG_ENDIAN ? "big-endian" : "little-endian", rec ? ", recompilador" : "");
	print(line);
	if (rec) {
		const struct salsa64_rec_stats *st = &rec->stats;
		snprintf(line, sizeof line, "recompilador: %llu bloques (%llu fuera de KSEG0), %llu recortados, "
			 "%llu solo intérprete, %llu entradas, %llu enlaces, %llu invalidados, %llu instr. interpretadas, "
			 "%llu llamadas al intérprete",
			 (unsigned long long)st->compiled, (unsigned long long)st->compiled_x,
			 (unsigned long long)st->temp_compiled,
			 (unsigned long long)st->interp_only, (unsigned long long)st->entries,
			 (unsigned long long)st->linked, (unsigned long long)st->invalidated,
			 (unsigned long long)st->interp_steps, (unsigned long long)st->exec_calls);
		print(line);
	}
	return t_failed_tests;
}

int salsa64_unit_run(void (*print)(const char *line))
{
	return run_suites(print, NULL, NULL, 0);
}

int salsa64_unit_run_rec(void (*print)(const char *line), struct salsa64_rec *rec, void *code, size_t bytes)
{
	return run_suites(print, rec, code, bytes);
}
