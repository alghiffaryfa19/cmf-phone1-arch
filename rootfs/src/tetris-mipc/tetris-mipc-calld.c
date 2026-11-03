// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Incoming calls on the CMF Phone 1's MediaTek modem without the Android RIL.
 *
 * The modem runs in MIPC mode (ro.vendor.mtk_mipc_support=1): for every incoming call
 * it sends CALL_APPROVE_INCOMING_CMD on the MIPC channel (/dev/ttyCMIPC0) and drops
 * the call unless the host answers CALL_APPROVE_INCOMING_RSP. Nothing on the AT
 * channel says so, the call just never rings. This daemon approves every incoming
 * call through the vendor libmipc.so (loaded with libhybris), so the modem goes on
 * and reports the call on the AT channel, where ModemManager handles it.
 *
 * Message and TLV ids from the vendor RIL (libmtkmipc-ril.so):
 *   CALL_APPROVE_INCOMING_CMD 0x8901: 0x100 call id (u32), 0x8101 number (str),
 *                                     0x102 toa, 0x103 seq no, 0x104 mode (u32)
 *   CALL_APPROVE_INCOMING_RSP 0x8902: 0x100 approve (u8), 0x102 call id, 0x103 seq no
 *                                     (u32), same transaction id as the command
 *   CALL_STATUS_IND           0x4901: logged, to see what the modem does next
 * mipc_msg_t header: sim/ps byte at +8, message id at +0xa, transaction id at +0xc.
 *
 * STATUS (2026-11-03): work in progress, not installed. The approval command never
 * arrived in tests: the modem falls back to 2G (CSFB) for ~30 s, then reports the call
 * (CALL_STATUS_IND msgType 134, then 1/129/133, cause 16) only as it is released.
 * TETRIS_MIPC_SNIFF=1 logs every modem command/indication; SYS_AT_IND (0x4001) arrives
 * at the CSFB moments, but its text is not in TLVs 0x100-0x10f/0x8100-0x810f.
 *
 * Build and run on the phone (as root):
 *   gcc -O2 -o tetris-mipc-calld tetris-mipc-calld.c -I/opt/libhybris/include \
 *       /opt/libhybris/lib/libhybris-common.so.1 -Wl,-rpath,/opt/libhybris/lib
 *   HYBRIS_LD_LIBRARY_PATH=/vendor/lib64:/system/lib64 ./tetris-mipc-calld
 */
#include <dlfcn.h>
#include <hybris/common/dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CALL_APPROVE_INCOMING_CMD 0x8901
#define CALL_APPROVE_INCOMING_RSP 0x8902
#define CALL_STATUS_IND           0x4901
#define MIPC_SIM0                 1

typedef struct mipc_msg mipc_msg_t;
typedef void (*mipc_cb)(mipc_msg_t *msg, void *priv);

static int (*mipc_init)(const char *name);
static int (*register_cmd)(uint16_t id, mipc_cb cb, void *priv, void *unused);
static int (*register_ind)(uint8_t sim_ps, uint16_t id, mipc_cb cb, void *priv, void *unused);
static mipc_msg_t *(*msg_init_with_txid)(uint16_t id, uint8_t sim_ps, uint16_t txid);
static int (*msg_add_tlv)(mipc_msg_t *msg, uint16_t type, uint16_t len, const void *val);
static void *(*msg_get_val_ptr)(mipc_msg_t *msg, uint16_t type, uint16_t *len);
static int (*msg_rsp)(mipc_msg_t *msg);
static void (*msg_deinit)(mipc_msg_t *msg);

static uint32_t get_u32(mipc_msg_t *msg, uint16_t type, uint32_t def)
{
	uint32_t *v = msg_get_val_ptr(msg, type, NULL);

	return v ? *v : def;
}

static void on_approve_incoming(mipc_msg_t *cmd, void *priv)
{
	uint8_t sim_ps = *((uint8_t *)cmd + 0x8), approve = 1;
	uint16_t txid = *(uint16_t *)((uint8_t *)cmd + 0xc);
	uint32_t call_id = get_u32(cmd, 0x100, 0), seq = get_u32(cmd, 0x103, 0);
	uint32_t mode = get_u32(cmd, 0x104, 0);
	mipc_msg_t *rsp;

	printf("incoming call: id %u seq %u mode %u sim/ps 0x%x txid %u -> approve\n",
	       call_id, seq, mode, sim_ps, txid);
	rsp = msg_init_with_txid(CALL_APPROVE_INCOMING_RSP, sim_ps, txid);
	if (!rsp) {
		printf("can't create response\n");
		return;
	}
	msg_add_tlv(rsp, 0x100, 1, &approve);
	msg_add_tlv(rsp, 0x102, 4, &call_id);
	msg_add_tlv(rsp, 0x103, 4, &seq);
	printf("response sent: %d\n", msg_rsp(rsp));
	msg_deinit(rsp);
}

static const char *(*id_to_str)(unsigned int id);

/* Debug aid (TETRIS_MIPC_SNIFF=1): log every modem command and call indication */
static void on_any(mipc_msg_t *msg, void *priv)
{
	uint16_t id = *(uint16_t *)((uint8_t *)msg + 0xa);

	printf("mipc 0x%04x %s\n", id, id_to_str ? id_to_str(id) : "");
	/* SYS_AT_IND carries AT URCs over MIPC: print its text TLVs, long digit runs
	 * (phone numbers) masked */
	if (id == 0x4001) {
		for (uint32_t i = 0; i < 0x20; i++) {
			uint16_t t = i < 0x10 ? 0x100 + i : 0x8100 + i - 0x10;
			uint16_t len = 0;
			char *v = msg_get_val_ptr(msg, t, &len), line[512];
			int digits = 0;

			if (!v || !len)
				continue;
			for (int i = 0; i < len && i < (int)sizeof(line) - 1; i++) {
				char c = v[i];

				digits = c >= '0' && c <= '9' ? digits + 1 : 0;
				line[i] = c < 0x20 || c > 0x7e ? '.' : digits > 6 ? '#' : c;
				line[i + 1] = 0;
			}
			printf("  at 0x%x: %s\n", t, line);
		}
	}
}

static void on_call_status(mipc_msg_t *ind, void *priv)
{
	printf("call status: ");
	for (uint16_t t = 0x100; t < 0x110; t++) {
		uint16_t len = 0;
		uint8_t *v = msg_get_val_ptr(ind, t, &len);

		if (v && len == 4)
			printf("%x=%u ", t, *(uint32_t *)v);
		else if (v && len == 1)
			printf("%x=%u ", t, *v);
		else if (v)
			printf("%x=<%u bytes> ", t, len);
	}
	printf("\n");
}

#define SYM(var, name) do { \
		*(void **)&var = hybris_dlsym(lib, name); \
		if (!var) { \
			fprintf(stderr, "missing %s\n", name); \
			return 1; \
		} \
	} while (0)

int main(void)
{
	void *lib;
	int ret;

	setvbuf(stdout, NULL, _IOLBF, 0);
	lib = hybris_dlopen("libmipc.so", RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "libmipc.so: %s\n", hybris_dlerror());
		return 1;
	}
	SYM(mipc_init, "mipc_init");
	SYM(register_cmd, "mipc_msg_register_cmd_api");
	SYM(register_ind, "mipc_msg_register_ind_api");
	SYM(msg_init_with_txid, "mipc_msg_init_with_txid");
	SYM(msg_add_tlv, "mipc_msg_add_tlv");
	SYM(msg_get_val_ptr, "mipc_msg_get_val_ptr");
	SYM(msg_rsp, "mipc_msg_rsp");
	SYM(msg_deinit, "mipc_msg_deinit");
	SYM(id_to_str, "mipcIdToString");

	ret = mipc_init(getenv("TETRIS_MIPC_NAME") ? getenv("TETRIS_MIPC_NAME") : "tetris-mipc-calld");
	printf("mipc_init: %d\n", ret);
	printf("register approve-incoming: %d\n",
	       register_cmd(CALL_APPROVE_INCOMING_CMD, on_approve_incoming, NULL, NULL));
	printf("register call-status: %d\n",
	       register_ind(MIPC_SIM0, CALL_STATUS_IND, on_call_status, NULL, NULL));
	if (getenv("TETRIS_MIPC_SNIFF")) {
		const char *unknown = id_to_str(0xffff);

		for (unsigned int id = 0x8000; id < 0x9000; id++)
			if (id != CALL_APPROVE_INCOMING_CMD && strcmp(id_to_str(id), unknown))
				register_cmd(id, on_any, NULL, NULL);
		for (unsigned int id = 0x4000; id < 0x5000; id++)
			if (id != CALL_STATUS_IND && strcmp(id_to_str(id), unknown))
				register_ind(MIPC_SIM0, id, on_any, NULL, NULL);
	}
	for (;;)
		pause();
}
