/*
 * Copyright (c) 2026 Xaloqi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Self-test for drivers/can/can_virtio.c against a real virtio-can device
 * (vhost-device-can in DomD). Prints ZCAN lines; DomD's script judges them.
 * DomD sends, after "ZCAN READY": 321#DEADBEEF, 18DAF110#0102030405,
 * 555#55 (no filter: must not arrive), 7E0#0211 (masked filter 0x7E0/0x7F0).
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/printk.h>

static const struct device *can = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
static int fails;

#define CHECK(cond, ...)                                                                           \
	do {                                                                                       \
		if (cond) {                                                                        \
			printk("ZCAN ok   " __VA_ARGS__);                                          \
		} else {                                                                           \
			printk("ZCAN FAIL " __VA_ARGS__);                                          \
			fails++;                                                                   \
		}                                                                                  \
	} while (0)

CAN_MSGQ_DEFINE(rxq, 16);
CAN_MSGQ_DEFINE(echoq, 4); /* own TX id 0x123: anything here was looped back by the device side */

static void print_frame(const char *tag, const struct can_frame *f)
{
	printk("ZCAN %s %0*X [%u]", tag, (f->flags & CAN_FRAME_IDE) ? 8 : 3, f->id, f->dlc);
	for (int i = 0; i < can_dlc_to_bytes(f->dlc); i++) {
		printk(" %02X", f->data[i]);
	}
	printk("\n");
}

static struct k_sem tx_done;
static volatile int tx_err = 1;

static void tx_cb(const struct device *dev, int error, void *user_data)
{
	tx_err = error;
	k_sem_give(&tx_done);
}

int main(void)
{
	struct can_frame f;
	can_mode_t cap;
	enum can_state st;
	struct can_bus_err_cnt ec;
	int ret;

	k_sem_init(&tx_done, 0, 1);
	printk("ZCAN start, device %s ready=%d\n", can->name, device_is_ready(can));
	if (!device_is_ready(can)) {
		printk("ZCAN RESULT FAIL (device not ready)\n");
		return 0;
	}

	ret = can_get_capabilities(can, &cap);
	CHECK(ret == 0 && (cap & ~CAN_MODE_FD) == CAN_MODE_NORMAL, "get_capabilities -> %d cap=0x%x\n", ret, cap);
	ret = can_set_bitrate(can, 500000);
	CHECK(ret == 0, "set_bitrate(500k) -> %d (accepted, owned by device side)\n", ret);
	ret = can_set_mode(can, CAN_MODE_LOOPBACK);
	CHECK(ret == -ENOTSUP, "set_mode(LOOPBACK) -> %d (expect -ENOTSUP)\n", ret);
	ret = can_set_mode(can, CAN_MODE_NORMAL);
	CHECK(ret == 0, "set_mode(NORMAL) -> %d\n", ret);
	ret = can_get_state(can, &st, &ec);
	CHECK(ret == 0 && st == CAN_STATE_STOPPED, "state before start = %d (expect STOPPED=%d)\n",
	      st, CAN_STATE_STOPPED);

	f = (struct can_frame){.id = 0x123, .dlc = 1};
	ret = can_send(can, &f, K_MSEC(100), NULL, NULL);
	CHECK(ret == -ENETDOWN, "send while stopped -> %d (expect -ENETDOWN)\n", ret);

	const struct can_filter f_std = {.id = 0x321, .mask = CAN_STD_ID_MASK};
	const struct can_filter f_ext = {.id = 0x18DAF110, .mask = CAN_EXT_ID_MASK,
					 .flags = CAN_FILTER_IDE};
	const struct can_filter f_mask = {.id = 0x7E0, .mask = 0x7F0};
	int id1 = can_add_rx_filter_msgq(can, &rxq, &f_std);
	int id2 = can_add_rx_filter_msgq(can, &rxq, &f_ext);
	int id3 = can_add_rx_filter_msgq(can, &rxq, &f_mask);
	const struct can_filter f_echo = {.id = 0x123, .mask = CAN_STD_ID_MASK};

	(void)can_add_rx_filter_msgq(can, &echoq, &f_echo);

	CHECK(id1 >= 0 && id2 >= 0 && id3 >= 0, "filters %d %d %d\n", id1, id2, id3);

	ret = can_start(can);
	CHECK(ret == 0, "start -> %d\n", ret);
	ret = can_start(can);
	CHECK(ret == -EALREADY, "start again -> %d (expect -EALREADY)\n", ret);
	ret = can_get_state(can, &st, &ec);
	CHECK(ret == 0 && st == CAN_STATE_ERROR_ACTIVE, "state after start = %d\n", st);

	/* TX: blocking (callback NULL), then with a callback, std + ext + 0-byte. */
	f = (struct can_frame){.id = 0x123, .dlc = 8,
			       .data = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}};
	ret = can_send(can, &f, K_MSEC(100), NULL, NULL);
	CHECK(ret == 0, "send 123 (blocking) -> %d\n", ret);
	f = (struct can_frame){.id = 0x1ABCDE01, .flags = CAN_FRAME_IDE, .dlc = 2,
			       .data = {0xAA, 0xBB}};
	ret = can_send(can, &f, K_MSEC(100), tx_cb, NULL);
	k_sem_take(&tx_done, K_SECONDS(2));
	CHECK(ret == 0 && tx_err == 0, "send 1ABCDE01 (callback) -> %d, cb err %d\n", ret, tx_err);
	f = (struct can_frame){.id = 0x7FF, .dlc = 0};
	ret = can_send(can, &f, K_MSEC(100), NULL, NULL);
	CHECK(ret == 0, "send 7FF [0] -> %d\n", ret);
	/* A burst larger than the TX slots, to exercise slot reuse. */
	int burst_ok = 0;

	for (int i = 0; i < 20; i++) {
		f = (struct can_frame){.id = 0x600 + i, .dlc = 1, .data = {i}};
		if (can_send(can, &f, K_MSEC(500), NULL, NULL) == 0) {
			burst_ok++;
		}
	}
	CHECK(burst_ok == 20, "burst 600..613: %d/20 sent\n", burst_ok);

	printk("ZCAN READY\n");

	/* RX: expect exactly 321, 18DAF110, 7E0 (and not 555) within 20 s. */
	int got321 = 0, got18 = 0, got7e0 = 0, other = 0;
	int64_t end = k_uptime_get() + 20000;

	while (k_uptime_get() < end) {
		if (k_msgq_get(&rxq, &f, K_MSEC(200)) != 0) {
			if (got321 && got18 && got7e0 && k_uptime_get() > end - 15000) {
				break;
			}
			continue;
		}
		print_frame("RX", &f);
		if (!(f.flags & CAN_FRAME_IDE) && f.id == 0x321 && f.dlc == 4 &&
		    f.data[0] == 0xDE && f.data[3] == 0xEF) {
			got321++;
		} else if ((f.flags & CAN_FRAME_IDE) && f.id == 0x18DAF110 && f.dlc == 5 &&
			   f.data[4] == 0x05) {
			got18++;
		} else if (!(f.flags & CAN_FRAME_IDE) && f.id == 0x7E0) {
			got7e0++;
		} else {
			other++;
		}
	}
	CHECK(got321 == 1, "RX 321 x%d\n", got321);
	CHECK(got18 == 1, "RX 18DAF110 x%d (extended)\n", got18);
	CHECK(got7e0 == 1, "RX 7E0 x%d (masked filter)\n", got7e0);
	CHECK(other == 0, "RX unexpected frames: %d (555 must be filtered out)\n", other);

	printk("ZCAN info own frame 123 received back %u time(s) (device-side loopback)\n",
	       k_msgq_num_used_get(&echoq));
	can_remove_rx_filter(can, id1);
	ret = can_stop(can);
	CHECK(ret == 0, "stop -> %d\n", ret);
	ret = can_get_state(can, &st, &ec);
	CHECK(st == CAN_STATE_STOPPED, "state after stop = %d\n", st);
	f = (struct can_frame){.id = 0x123, .dlc = 1};
	ret = can_send(can, &f, K_MSEC(100), NULL, NULL);
	CHECK(ret == -ENETDOWN, "send after stop -> %d\n", ret);
	ret = can_start(can);
	CHECK(ret == 0, "restart -> %d\n", ret);
	f = (struct can_frame){.id = 0x124, .dlc = 1, .data = {0x42}};
	ret = can_send(can, &f, K_MSEC(100), NULL, NULL);
	CHECK(ret == 0, "send 124 after restart -> %d\n", ret);

	printk("ZCAN RESULT %s (%d failure(s))\n", fails ? "FAIL" : "PASS", fails);
	return 0;
}
