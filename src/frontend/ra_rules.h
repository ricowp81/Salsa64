/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — reglas del modo Hardcore de RetroAchievements, como funciones
 * puras (sin red ni estado): qué acciones valen con Hardcore activo y cómo
 * cambia el modo con un juego abierto. docs/retroachievements.md.
 */
#ifndef SALSA64_FRONTEND_RA_RULES_H
#define SALSA64_FRONTEND_RA_RULES_H

#include <stdbool.h>

enum ra_rule_action {
	RA_RULE_STATE_SAVE,    /* guardar un estado: vale (para depurar) */
	RA_RULE_STATE_LOAD,    /* cargar un estado (menú, atajo o prueba): nunca */
	RA_RULE_CHEATS,        /* trucos y parches de memoria: nunca */
	RA_RULE_REWIND,        /* rebobinar: nunca (Salsa64 no lo tiene) */
	RA_RULE_SLOWMO,        /* cámara lenta o ritmo reducido: nunca (no existe) */
	RA_RULE_FRAME_STEP,    /* avance cuadro por cuadro: nunca (no existe) */
	RA_RULE_FAST_FORWARD,  /* adelanto rápido («turbo»): vale */
	RA_RULE_GRAPHICS,      /* filtros, escala, interpolación, aspecto: valen */
	RA_RULE_ACTIONS
};

/* ¿Se permite `a` mientras `hardcore` (un juego con logros y Hardcore)? */
bool ra_rules_allowed(bool hardcore, enum ra_rule_action a);

/*
 * El ajuste Hardcore cambió con (o sin) un juego abierto. `game_hc` es el modo
 * con que arrancó el juego abierto. Devuelve el modo con que sigue ese juego:
 * - sin juego abierto: el ajuste vale tal cual (para el siguiente juego);
 * - pasar a Hardcore con el juego abierto no se permite sin reiniciarlo: el
 *   juego sigue como estaba y el ajuste vale al abrir el siguiente
 *   (`*deferred` = true);
 * - dejar Hardcore con el juego abierto vale al instante.
 */
bool ra_rules_mode_change(bool game_open, bool game_hc, bool want_hc, bool *deferred);

#endif
