/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Extension detection is shared by the browser and the host unit tests. */
#include "test.h"
#include "frontend/romlist.h"

static void test_archive_extensions(void)
{
    CHECK(romlist_has_rom_ext("game.z64"));
    CHECK(romlist_has_rom_ext("game.V64"));
    CHECK(romlist_has_rom_ext("game.n64"));
    CHECK(romlist_has_rom_ext("collection.zip"));
    CHECK(romlist_has_rom_ext("collection.RAR"));
    CHECK(romlist_has_rom_ext("collection.7Z"));
    CHECK(!romlist_has_rom_ext("collection.rar.old"));
    CHECK(!romlist_has_rom_ext("readme.txt"));
}

void t_suite_archiverom(void)
{
    RUN(test_archive_extensions);
}
