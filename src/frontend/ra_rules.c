/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Salsa64 — reglas del modo Hardcore. Ver ra_rules.h. */
#include "ra_rules.h"

bool ra_rules_allowed(bool hardcore, enum ra_rule_action a)
{
	if (!hardcore)
		return true;
	switch (a) {
	case RA_RULE_STATE_SAVE:
	case RA_RULE_FAST_FORWARD:
	case RA_RULE_GRAPHICS:
		return true;
	default:
		return false;  /* cargar, trucos, rebobinar, lento, cuadro a cuadro y lo desconocido */
	}
}

bool ra_rules_mode_change(bool game_open, bool game_hc, bool want_hc, bool *deferred)
{
	if (deferred)
		*deferred = false;
	if (!game_open)
		return want_hc;
	if (want_hc && !game_hc) {
		if (deferred)
			*deferred = true;
		return false;
	}
	return want_hc;  /* igual, o Hardcore → normal al instante */
}
