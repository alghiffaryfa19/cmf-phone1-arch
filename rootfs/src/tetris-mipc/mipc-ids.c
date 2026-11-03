// SPDX-License-Identifier: GPL-2.0-or-later
/* Dumps MediaTek MIPC message ids and names from the vendor libmipc.so (via libhybris). */
#include <dlfcn.h>
#include <hybris/common/dlfcn.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	void *lib = hybris_dlopen("libmipc.so", RTLD_NOW);
	const char *(*id2str)(unsigned int);
	const char *unknown;

	if (!lib) {
		fprintf(stderr, "%s\n", hybris_dlerror());
		return 1;
	}
	*(void **)&id2str = hybris_dlsym(lib, "mipcIdToString");
	unknown = id2str(0xffff);
	for (unsigned int id = 0; id < 0x10000; id++) {
		const char *s = id2str(id);

		if (s && s != unknown && strcmp(s, unknown))
			printf("0x%04x %s\n", id, s);
	}
	return 0;
}
