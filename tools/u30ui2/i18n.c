/* i18n: the string tables and the language file. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include "i18n.h"

#define LANG_FILE "/etc/mu5358/lang"

static const char *const strings[NLANG][NSTRINGS] = {
#define X(id, en, tr, zh) en,
	{ STRINGS(X) },
#undef X
#define X(id, en, tr, zh) tr,
	{ STRINGS(X) },
#undef X
#define X(id, en, tr, zh) zh,
	{ STRINGS(X) },
#undef X
};
static const char *const codes[NLANG] = { "en", "tr", "zh" };

int lang = L_EN;

const char *T(int id)
{
	return id >= 0 && id < NSTRINGS ? strings[lang][id] : "";
}

void lang_load(void)
{
	FILE *f = fopen(LANG_FILE, "r");
	char b[16] = "";
	int i;

	if (f) {
		if (!fgets(b, sizeof(b), f))
			b[0] = 0;
		fclose(f);
	}
	for (i = 0; i < NLANG; i++)
		if (!strncmp(b, codes[i], 2))
			lang = i;
}

void lang_save(void)
{
	FILE *f = fopen(LANG_FILE, "w");

	if (f) {
		fprintf(f, "%s\n", codes[lang]);
		fclose(f);
	}
}
