/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pruebas del lector de ROMs en ZIP: entradas almacenadas y Deflate, rechazo por CRC y archivos dañados. */
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "frontend/ziprom.h"

/* Inyecta falta de memoria en la misma implementación, sin alterar el
 * asignador de las demás pruebas ni el de las ROMs sintéticas. */
static unsigned zip_fail_stage;
static void *zip_test_malloc(size_t n)
{
	return zip_fail_stage == 1 ? NULL : malloc(n);
}
static int zip_test_inflate_init(z_streamp zs, int bits, const char *version, int size)
{
	return zip_fail_stage == 2 ? Z_MEM_ERROR : inflateInit2_(zs, bits, version, size);
}
static int zip_test_inflate(z_streamp zs, int flush)
{
	return zip_fail_stage == 3 ? Z_MEM_ERROR : inflate(zs, flush);
}
#define malloc zip_test_malloc
#define inflateInit2_ zip_test_inflate_init
#define inflate zip_test_inflate
#define salsa64_ziprom_is_path zip_test_is_path
#define salsa64_ziprom_read_header zip_test_read_header
#define salsa64_ziprom_extract zip_test_extract
#include "frontend/ziprom.c"
#undef salsa64_ziprom_extract
#undef salsa64_ziprom_read_header
#undef salsa64_ziprom_is_path
#undef inflate
#undef inflateInit2_
#undef malloc

#define TEST_ROM_SIZE 4096u

static uint8_t test_rom[TEST_ROM_SIZE];

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void init_test_rom(void)
{
	for (uint32_t i = 0; i < sizeof test_rom; i++)
		test_rom[i] = (uint8_t)(i * 37u + (i >> 3) + 11u);
	memcpy(test_rom + 0x20, "ZIP TEST ROM", 12);
}

static bool write_test_zip(const char *member, unsigned method, uint16_t flags, bool bad_crc,
                           char path[64])
{
	uint8_t compressed[TEST_ROM_SIZE + 128];
	const uint8_t *payload = test_rom;
	size_t payload_len = sizeof test_rom;
	size_t compressed_len = payload_len;
	uLong sum = crc32(0L, Z_NULL, 0);
	for (size_t off = 0; off < payload_len;) {
		size_t n = payload_len - off;
		if (n > 16384)
			n = 16384;
		sum = crc32(sum, payload + off, (uInt)n);
		off += n;
	}
	uint32_t crc = (uint32_t)sum;
	if (method == 8) {
		z_stream zs;
		memset(&zs, 0, sizeof zs);
		if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
		                 Z_DEFAULT_STRATEGY) != Z_OK)
			return false;
		zs.next_in = (Bytef *)payload;
		zs.avail_in = (uInt)payload_len;
		zs.next_out = compressed;
		zs.avail_out = sizeof compressed;
		int zr = deflate(&zs, Z_FINISH);
		compressed_len = zs.total_out;
		deflateEnd(&zs);
		if (zr != Z_STREAM_END || !compressed_len)
			return false;
		payload = compressed;
	} else if (method != 0) {
		return false;
	}

	size_t name_len = strlen(member);
	if (!name_len || name_len > 250)
		return false;
	size_t local_size = 30 + name_len + compressed_len;
	size_t central_size = 46 + name_len;
	size_t total = local_size + central_size + 22;
	uint8_t *archive = (uint8_t *)calloc(1, total);
	if (!archive)
		return false;
	uint8_t *local = archive;
	put32(local, 0x04034B50u);
	put16(local + 4, 20);
	put16(local + 6, flags);
	put16(local + 8, (uint16_t)method);
	put32(local + 14, crc);
	put32(local + 18, (uint32_t)compressed_len);
	put32(local + 22, (uint32_t)payload_len);
	put16(local + 26, (uint16_t)name_len);
	memcpy(local + 30, member, name_len);
	memcpy(local + 30 + name_len, payload, compressed_len);

	uint8_t *central = archive + local_size;
	put32(central, 0x02014B50u);
	put16(central + 4, 20);
	put16(central + 6, 20);
	put16(central + 8, flags);
	put16(central + 10, (uint16_t)method);
	put32(central + 16, crc ^ (bad_crc ? 1u : 0u));
	put32(central + 20, (uint32_t)compressed_len);
	put32(central + 24, (uint32_t)payload_len);
	put16(central + 28, (uint16_t)name_len);
	put32(central + 42, 0);
	memcpy(central + 46, member, name_len);

	uint8_t *eocd = central + central_size;
	put32(eocd, 0x06054B50u);
	put16(eocd + 8, 1);
	put16(eocd + 10, 1);
	put32(eocd + 12, (uint32_t)central_size);
	put32(eocd + 16, (uint32_t)local_size);

	memcpy(path, "/tmp/salsa64-ziprom-XXXXXX", 27);
	path[27] = '\0';
	int fd = mkstemp(path);
	if (fd < 0) {
		free(archive);
		return false;
	}
	close(fd);
	FILE *f = fopen(path, "wb");
	bool ok = f && fwrite(archive, 1, total, f) == total;
	if (f && fclose(f) != 0)
		ok = false;
	free(archive);
	if (!ok)
		unlink(path);
	return ok;
}

static void ziprom_stored_roundtrip(void)
{
	char path[64];
	uint8_t header[SALSA64_ROM_HEADER_SIZE];
	uint8_t actual[TEST_ROM_SIZE];
	uint32_t size = 0;
	char member[256];

	init_test_rom();
	CHECK(write_test_zip("games/test.z64", 0, 0, false, path));
	CHECK_EQ(salsa64_ziprom_read_header(path, header, &size, member, sizeof member), 0);
	CHECK_EQ(size, TEST_ROM_SIZE);
	CHECK(memcmp(header, test_rom, sizeof header) == 0);
	CHECK(strcmp(member, "games/test.z64") == 0);
	CHECK_EQ(salsa64_ziprom_extract(path, actual, sizeof actual, &size, member, sizeof member), 0);
	CHECK_EQ(size, TEST_ROM_SIZE);
	CHECK(memcmp(actual, test_rom, sizeof actual) == 0);
	unlink(path);
}

static void ziprom_deflate_roundtrip(void)
{
	char path[64];
	uint8_t header[SALSA64_ROM_HEADER_SIZE];
	uint8_t actual[TEST_ROM_SIZE];
	uint32_t size = 0;

	init_test_rom();
	CHECK(write_test_zip("folder/Test.V64", 8, 0, false, path));
	CHECK_EQ(salsa64_ziprom_read_header(path, header, &size, NULL, 0), 0);
	CHECK_EQ(size, TEST_ROM_SIZE);
	CHECK(memcmp(header, test_rom, sizeof header) == 0);
	CHECK_EQ(salsa64_ziprom_extract(path, actual, sizeof actual, &size, NULL, 0), 0);
	CHECK(memcmp(actual, test_rom, sizeof actual) == 0);
	unlink(path);
}

static void ziprom_bad_crc_rejected(void)
{
	char path[64];
	uint8_t header[SALSA64_ROM_HEADER_SIZE];
	uint8_t actual[TEST_ROM_SIZE];
	uint32_t size = 0;

	init_test_rom();
	CHECK(write_test_zip("test.n64", 8, 0, true, path));
	/* La vista previa de la cabecera es barata; la integridad se exige antes de arrancar. */
	CHECK_EQ(salsa64_ziprom_read_header(path, header, &size, NULL, 0), 0);
	CHECK_EQ(salsa64_ziprom_extract(path, actual, sizeof actual, &size, NULL, 0), -1);
	unlink(path);
}

static void ziprom_encrypted_and_invalid_rejected(void)
{
	char path[64];
	uint8_t header[SALSA64_ROM_HEADER_SIZE];
	uint32_t size = 0;

	init_test_rom();
	CHECK(write_test_zip("locked.z64", 8, 1, false, path));
	CHECK_EQ(salsa64_ziprom_read_header(path, header, &size, NULL, 0), -1);
	unlink(path);

	memcpy(path, "/tmp/salsa64-ziprom-XXXXXX", 27);
	path[27] = '\0';
	int fd = mkstemp(path);
	CHECK(fd >= 0);
	if (fd >= 0) {
		static const char junk[] = "not a zip archive";
		CHECK(write(fd, junk, sizeof junk) == (ssize_t)sizeof junk);
		close(fd);
		CHECK_EQ(salsa64_ziprom_read_header(path, header, &size, NULL, 0), -1);
		unlink(path);
	}
}

static void ziprom_path_extensions(void)
{
	CHECK(salsa64_ziprom_is_path("game.zip"));
	CHECK(salsa64_ziprom_is_path("/roms/GAME.ZIP"));
	CHECK(!salsa64_ziprom_is_path("game.z64"));
	CHECK(!salsa64_ziprom_is_path("game.zip.bak"));
	CHECK(!salsa64_ziprom_is_path(NULL));
}

static void ziprom_memory_errors(void)
{
	char path[64];
	uint8_t actual[TEST_ROM_SIZE], header[SALSA64_ROM_HEADER_SIZE];
	uint32_t size;
	init_test_rom();
	CHECK(write_test_zip("memory.z64", 8, 0, false, path));
	for (zip_fail_stage = 1; zip_fail_stage <= 3; zip_fail_stage++) {
		CHECK_EQ(zip_test_read_header(path, header, &size, NULL, 0), SALSA64_ZIPROM_NOMEM);
		CHECK_EQ(zip_test_extract(path, actual, sizeof actual, &size, NULL, 0), SALSA64_ZIPROM_NOMEM);
	}
	zip_fail_stage = 0;
	CHECK_EQ(zip_test_extract(path, actual, sizeof actual, &size, NULL, 0), 0);
	unlink(path);
	/* Un error de archivo posterior no conserva el error de memoria. */
	CHECK_EQ(zip_test_read_header(path, header, &size, NULL, 0), -1);
	CHECK(write_test_zip("crc.z64", 8, 0, true, path));
	CHECK_EQ(zip_test_extract(path, actual, sizeof actual, &size, NULL, 0), -1);
	unlink(path);
}

void t_ziprom_suite(void)
{
	RUN(ziprom_memory_errors);
	RUN(ziprom_stored_roundtrip);
	RUN(ziprom_deflate_roundtrip);
	RUN(ziprom_bad_crc_rejected);
	RUN(ziprom_encrypted_and_invalid_rejected);
	RUN(ziprom_path_extensions);
}
