// SPDX-License-Identifier: MIT
/*
 * The screen lock's PIN (D044): only a salted hash of it is kept, as
 * lock_pin in the settings (crypt(3), libxcrypt's default method). Settings
 * sets it, s22-lock checks it.
 */
#define _GNU_SOURCE
#include <crypt.h>
#include <stdio.h>
#include <string.h>
#include "s22ui.h"

bool ui_pin_set_up(void)
{
	char v[256], h[256];

	return !strcmp(ui_setting("screen_lock", "none", v, sizeof(v)), "pin") &&
	       ui_setting("lock_pin", "", h, sizeof(h))[0] == '$';
}

bool ui_pin_check(const char *pin)
{
	char hash[256];
	struct crypt_data cd;

	memset(&cd, 0, sizeof(cd));
	if (ui_setting("lock_pin", "", hash, sizeof(hash))[0] != '$' || !pin[0])
		return false;
	const char *got = crypt_r(pin, hash, &cd);
	bool ok = got && got[0] == '$' && strcmp(got, hash) == 0;
	explicit_bzero(&cd, sizeof(cd));
	return ok;
}

bool ui_pin_set(const char *pin)
{
	struct crypt_data cd;
	char salt[CRYPT_GENSALT_OUTPUT_SIZE];
	bool ok = false;

	memset(&cd, 0, sizeof(cd));
	if (!pin) {				/* no PIN: no lock */
		return ui_setting_set("screen_lock", "none") &&
		       ui_setting_set("lock_pin", NULL);
	}
	if (crypt_gensalt_rn(NULL, 0, NULL, 0, salt, sizeof(salt))) {
		const char *h = crypt_r(pin, salt, &cd);
		ok = h && h[0] == '$' && ui_setting_set("lock_pin", h) &&
		     ui_setting_set("screen_lock", "pin");
	}
	explicit_bzero(&cd, sizeof(cd));
	return ok;
}
