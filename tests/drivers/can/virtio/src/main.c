/*
 * Copyright (c) 2026 Xaloqi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for drivers/can/can_virtio.c against a fake virtio transport
 * (src/fake_virtio.c): the test plays the VIRTIO CAN device (VIRTIO 1.4,
 * "CAN Device"). Completions go through Zephyr's virtio_isr() in ISR context.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>
#include <zephyr/version.h>
#include "fake_virtio.h"

#define QTX   0
#define QRX   1
#define QCTRL 2

#define CTRL_START 0x0201
#define CTRL_STOP  0x0202
#define MSG_TX     0x0001
#define MSG_RX     0x0101
#define F_RTR      0x2000
#define F_FD       0x4000
#define F_EXT      0x8000

struct hdr {
	uint16_t msg_type;
	uint16_t length;
	uint8_t reserved_classic_dlc;
	uint8_t padding;
	uint16_t reserved_xl_priority;
	uint32_t flags;
	uint32_t can_id;
} __packed;

static const struct device *const can = DEVICE_DT_GET(DT_NODELABEL(can_dut));
static const struct device *const vdev = DEVICE_DT_GET(DT_NODELABEL(fake_virtio));

/* --- the fake device's behaviour ------------------------------------------ */

static bool ctrl_auto = true;
static uint8_t ctrl_result;
static uint16_t ctrl_last;
static int ctrl_seen;

static bool tx_auto = true;
static uint8_t tx_result;
static struct {
	struct hdr h;
	uint8_t sdu[64];
	uint32_t out_len;
} tx_log[32];
static int tx_seen;

static void complete_ctrl(uint8_t result)
{
	struct fake_chain c;

	zassert_true(fake_virtio_pop(vdev, QCTRL, &c), "no control request queued");
	zassert_equal(c.n, 2);
	zassert_false(c.writable[0]);
	zassert_true(c.writable[1]);
	zassert_equal(c.len[0], 2);
	ctrl_last = sys_le16_to_cpu(*(uint16_t *)c.addr[0]);
	*(uint8_t *)c.addr[1] = result;
	ctrl_seen++;
	fake_virtio_push_used(vdev, QCTRL, c.head, 1);
	fake_virtio_interrupt(vdev);
}

static int complete_tx(uint8_t result)
{
	struct fake_chain c;

	if (!fake_virtio_pop(vdev, QTX, &c)) {
		return -1;
	}
	zassert_equal(c.n, 2, "TX chain must be request + result");
	zassert_false(c.writable[0]);
	zassert_true(c.writable[1]);
	zassert_equal(c.len[1], 1);
	zassert_true(c.len[0] >= sizeof(struct hdr));
	int i = tx_seen % ARRAY_SIZE(tx_log);

	memcpy(&tx_log[i].h, c.addr[0], sizeof(struct hdr));
	memcpy(tx_log[i].sdu, (uint8_t *)c.addr[0] + sizeof(struct hdr), c.len[0] - sizeof(struct hdr));
	tx_log[i].out_len = c.len[0];
	tx_seen++;
	*(uint8_t *)c.addr[1] = result;
	fake_virtio_push_used(vdev, QTX, c.head, 1);
	fake_virtio_interrupt(vdev);
	return i;
}

static void notify_hook(const struct device *dev, uint16_t q)
{
	if (q == QCTRL && ctrl_auto) {
		complete_ctrl(ctrl_result);
	} else if (q == QTX && tx_auto) {
		(void)complete_tx(tx_result);
	}
}

/* Device side: deliver one frame into the next RX buffer; used_len lets a test lie. */
static void inject_rx_raw(uint16_t msg_type, uint32_t flags, uint32_t id, const uint8_t *sdu,
			  uint16_t len, int32_t used_len)
{
	struct fake_chain c;
	struct hdr h = {
		.msg_type = sys_cpu_to_le16(msg_type),
		.length = sys_cpu_to_le16(len),
		.flags = sys_cpu_to_le32(flags),
		.can_id = sys_cpu_to_le32(id),
	};

	zassert_true(fake_virtio_pop(vdev, QRX, &c), "no RX buffer available");
	zassert_equal(c.n, 1);
	zassert_true(c.writable[0]);
	zassert_true(c.len[0] >= sizeof(h) + len);
	memcpy(c.addr[0], &h, sizeof(h));
	memcpy((uint8_t *)c.addr[0] + sizeof(h), sdu, len);
	fake_virtio_push_used(vdev, QRX, c.head, used_len >= 0 ? (uint32_t)used_len : sizeof(h) + len);
	fake_virtio_interrupt(vdev);
}

static void inject_rx(uint32_t flags, uint32_t id, const uint8_t *sdu, uint16_t len)
{
	inject_rx_raw(MSG_RX, flags, id, sdu, len, -1);
}

/* --- driver-side observers ----------------------------------------------- */

CAN_MSGQ_DEFINE(rxq, 8);

static struct k_sem txcb_sem;
static int txcb_err;
static int txcb_calls;

static void tx_cb(const struct device *dev, int error, void *user_data)
{
	txcb_err = error;
	txcb_calls++;
	k_sem_give(&txcb_sem);
}

static enum can_state last_state = -1;
static int state_calls;

/*
 * Zephyr 4.5 replaced can_set_state_change_callback() (deprecated, an error under
 * -Werror) with a list of caller-owned callbacks. Same gate as drivers/can/can_virtio.c.
 */
#if ZEPHYR_VERSION_CODE >= ZEPHYR_VERSION(4, 5, 0)
#define STATE_CB_LIST 1
static struct can_state_change_callback cb_a, cb_b;
static int state_calls_b;

static void state_cb(const struct device *dev, struct can_state_change_callback *cb,
		     enum can_state state, struct can_bus_err_cnt ec)
{
	last_state = state;
	state_calls++;
}

static void state_cb_b(const struct device *dev, struct can_state_change_callback *cb,
		       enum can_state state, struct can_bus_err_cnt ec)
{
	state_calls_b++;
}

/* Two callbacks, so that a driver reporting to only one of them is caught. */
static void state_cb_register(void)
{
	state_calls_b = 0;
	can_init_state_change_callback(&cb_a, state_cb);
	can_init_state_change_callback(&cb_b, state_cb_b);
	zassert_ok(can_add_state_change_callback(can, &cb_a));
	zassert_ok(can_add_state_change_callback(can, &cb_b));
}

static void state_cb_unregister(void)
{
	zassert_ok(can_remove_state_change_callback(can, &cb_a));
	zassert_ok(can_remove_state_change_callback(can, &cb_b));
}
#else
static void state_cb(const struct device *dev, enum can_state state, struct can_bus_err_cnt ec,
		     void *user_data)
{
	last_state = state;
	state_calls++;
}

static void state_cb_register(void)
{
	can_set_state_change_callback(can, state_cb, NULL);
}

static void state_cb_unregister(void)
{
	can_set_state_change_callback(can, NULL, NULL);
}
#endif

static void reset_fixture(void *f)
{
	ARG_UNUSED(f);
	ctrl_auto = true;
	ctrl_result = 0;
	tx_auto = true;
	tx_result = 0;
	fake_virtio_config_status = 0;
	k_msgq_purge(&rxq);
	k_sem_reset(&txcb_sem);
	txcb_calls = 0;
	(void)can_stop(can);
	(void)can_set_mode(can, CAN_MODE_NORMAL);
}

static void *suite_setup(void)
{
	TC_PRINT("Zephyr %s, ZEPHYR_VERSION_CODE %#x, state change callbacks: %s\n", KERNEL_VERSION_STRING,
		 ZEPHYR_VERSION_CODE, IS_ENABLED(STATE_CB_LIST) ? "list (4.5)" : "single (4.4)");
	k_sem_init(&txcb_sem, 0, 32);
	fake_virtio_set_notify_hook(notify_hook);
	return NULL;
}

static void start(void)
{
	zassert_ok(can_start(can));
}

/* --- tests ------------------------------------------------------------------ */

ZTEST(can_virtio, test_init_negotiation_and_rx_population)
{
	zassert_true(device_is_ready(can));
	zassert_true(fake_virtio_features_ok);
	zassert_true(fake_virtio_driver_ok);
	zassert_true(fake_virtio_driver_features & BIT64(0), "CAN_CLASSIC not negotiated");
	zassert_true(fake_virtio_driver_features & BIT64(2), "RTR_FRAMES not negotiated");
	zassert_true(fake_virtio_driver_features & BIT64(3), "LATE_TX_ACK not negotiated");
	zassert_equal(!!(fake_virtio_driver_features & BIT64(1)), IS_ENABLED(CONFIG_CAN_FD_MODE),
		      "CAN_FD must be negotiated exactly when CONFIG_CAN_FD_MODE=y");
	/* "The driver MUST NOT send any buffer available notifications ... before DRIVER_OK" */
	zassert_equal(fake_virtio_notify_before_driver_ok, 0);
	/* "The driver MUST populate the Rxq with empty device-writeable buffers" */
	zassert_equal(fake_virtio_avail_count(vdev, QRX), fake_virtio_queue_size[QRX]);
	zassert_true(fake_virtio_queue_size[QRX] >= 2);
	zassert_true(fake_virtio_queue_size[QCTRL] >= 2);
}

ZTEST(can_virtio, test_rx_buffers_have_room_for_64_bytes)
{
	struct fake_chain c;

	/* Peek through the ring: every RX buffer is one writable 16 + 64 byte descriptor. */
	zassert_true(fake_virtio_pop(vdev, QRX, &c));
	zassert_equal(c.n, 1);
	zassert_true(c.writable[0]);
	zassert_equal(c.len[0], 16 + 64);
	/* Give it back used-but-empty: a malformed (short) buffer is dropped and re-queued. */
	uint16_t before = fake_virtio_avail_count(vdev, QRX);

	fake_virtio_push_used(vdev, QRX, c.head, 0);
	fake_virtio_interrupt(vdev);
	zassert_equal(fake_virtio_avail_count(vdev, QRX), before + 1);
}

ZTEST(can_virtio, test_modes_and_timing)
{
	can_mode_t cap;

	zassert_ok(can_get_capabilities(can, &cap));
	zassert_equal(cap & ~CAN_MODE_FD, CAN_MODE_NORMAL);
	zassert_equal(can_set_mode(can, CAN_MODE_LOOPBACK), -ENOTSUP);
	zassert_equal(can_set_mode(can, CAN_MODE_LISTENONLY), -ENOTSUP);
	zassert_ok(can_set_mode(can, CAN_MODE_NORMAL));
	/* No guest-side bit timing in VIRTIO 1.4: accepted and ignored while stopped. */
	zassert_ok(can_set_bitrate(can, 500000));
	start();
	zassert_equal(can_set_bitrate(can, 250000), -EBUSY);
	zassert_equal(can_set_mode(can, CAN_MODE_NORMAL), -EBUSY);
}

ZTEST(can_virtio, test_start_stop_control_queue)
{
	enum can_state st;

	zassert_ok(can_get_state(can, &st, NULL));
	zassert_equal(st, CAN_STATE_STOPPED);
	int seen = ctrl_seen;

	start();
	zassert_equal(ctrl_seen, seen + 1);
	zassert_equal(ctrl_last, CTRL_START);
	zassert_equal(can_start(can), -EALREADY);
	zassert_ok(can_get_state(can, &st, NULL));
	zassert_equal(st, CAN_STATE_ERROR_ACTIVE);
	zassert_ok(can_stop(can));
	zassert_equal(ctrl_last, CTRL_STOP);
	zassert_equal(can_stop(can), -EALREADY);
	zassert_ok(can_get_state(can, &st, NULL));
	zassert_equal(st, CAN_STATE_STOPPED);
}

ZTEST(can_virtio, test_start_rejected_by_device)
{
	enum can_state st;

	ctrl_result = 1; /* VIRTIO_CAN_RESULT_NOT_OK */
	zassert_equal(can_start(can), -EIO);
	zassert_ok(can_get_state(can, &st, NULL));
	zassert_equal(st, CAN_STATE_STOPPED);
	ctrl_result = 0;
	start();
}

ZTEST(can_virtio, test_control_timeout_does_not_reuse_buffers)
{
	/* A STOP the device does not answer: stop() times out, the request stays queued. */
	start();
	ctrl_auto = false;
	zassert_equal(can_stop(can), -EIO);
	zassert_equal(fake_virtio_avail_count(vdev, QCTRL), 1, "request must stay queued");
	/* A START while that STOP is still pending must not touch its buffers. */
	zassert_equal(can_start(can), -EIO);
	zassert_equal(fake_virtio_avail_count(vdev, QCTRL), 1, "no second request may be queued");
	/* The device answers late: what it reads must still be the STOP it was given. */
	complete_ctrl(0);
	zassert_equal(ctrl_last, CTRL_STOP, "pending request's buffer was overwritten");
	/* After the late answer the queue works normally again. */
	ctrl_auto = true;
	start();
	zassert_equal(ctrl_last, CTRL_START);
}

ZTEST(can_virtio, test_tx_encoding)
{
	struct can_frame f = {.id = 0x123, .dlc = 8,
			      .data = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}};
	int i;

	start();
	int base = tx_seen;

	zassert_ok(can_send(can, &f, K_MSEC(100), NULL, NULL));
	i = base % ARRAY_SIZE(tx_log);
	zassert_equal(sys_le16_to_cpu(tx_log[i].h.msg_type), MSG_TX);
	zassert_equal(sys_le16_to_cpu(tx_log[i].h.length), 8);
	zassert_equal(sys_le32_to_cpu(tx_log[i].h.flags), 0);
	zassert_equal(sys_le32_to_cpu(tx_log[i].h.can_id), 0x123);
	zassert_equal(tx_log[i].out_len, 16 + 8);
	zassert_mem_equal(tx_log[i].sdu, f.data, 8);

	f = (struct can_frame){.id = 0x1ABCDE01, .flags = CAN_FRAME_IDE, .dlc = 2, .data = {1, 2}};
	zassert_ok(can_send(can, &f, K_MSEC(100), NULL, NULL));
	i = (base + 1) % ARRAY_SIZE(tx_log);
	zassert_equal(sys_le32_to_cpu(tx_log[i].h.flags), F_EXT);
	zassert_equal(sys_le32_to_cpu(tx_log[i].h.can_id), 0x1ABCDE01);
	zassert_equal(tx_log[i].out_len, 16 + 2);

	f = (struct can_frame){.id = 0x7FF, .dlc = 0};
	zassert_ok(can_send(can, &f, K_MSEC(100), NULL, NULL));
	i = (base + 2) % ARRAY_SIZE(tx_log);
	zassert_equal(sys_le16_to_cpu(tx_log[i].h.length), 0);
	zassert_equal(tx_log[i].out_len, 16);

	/* RTR: the requested length, no payload bytes that mean anything. */
	f = (struct can_frame){.id = 0x456, .flags = CAN_FRAME_RTR, .dlc = 4};
	zassert_ok(can_send(can, &f, K_MSEC(100), NULL, NULL));
	i = (base + 3) % ARRAY_SIZE(tx_log);
	zassert_equal(sys_le32_to_cpu(tx_log[i].h.flags), F_RTR);
	zassert_equal(sys_le16_to_cpu(tx_log[i].h.length), 4);
}

ZTEST(can_virtio, test_tx_invalid_arguments)
{
	struct can_frame f = {.id = 0x123, .dlc = 1};

	zassert_equal(can_send(can, &f, K_NO_WAIT, NULL, NULL), -ENETDOWN, "stopped");
	start();
	f.dlc = CAN_MAX_DLC + 1;
	zassert_equal(can_send(can, &f, K_NO_WAIT, NULL, NULL), -EINVAL);
	f = (struct can_frame){.id = 0x123, .dlc = 1, .flags = CAN_FRAME_ESI};
	zassert_equal(can_send(can, &f, K_NO_WAIT, NULL, NULL), -ENOTSUP);
}

ZTEST(can_virtio, test_tx_callback_results)
{
	struct can_frame f = {.id = 0x10, .dlc = 1, .data = {0xAB}};

	start();
	zassert_ok(can_send(can, &f, K_MSEC(100), tx_cb, NULL));
	zassert_ok(k_sem_take(&txcb_sem, K_MSEC(100)));
	zassert_equal(txcb_err, 0);

	tx_result = 1; /* NOT_OK, no bus-off */
	zassert_ok(can_send(can, &f, K_MSEC(100), tx_cb, NULL));
	zassert_ok(k_sem_take(&txcb_sem, K_MSEC(100)));
	zassert_equal(txcb_err, -EIO);
	tx_result = 0;
}

ZTEST(can_virtio, test_tx_slots_exhaust_and_recover)
{
	struct can_frame f = {.id = 0x20, .dlc = 1};
	int queued = 0;

	start();
	tx_auto = false; /* the device holds every request */
	while (can_send(can, &f, K_NO_WAIT, tx_cb, NULL) == 0) {
		queued++;
		zassert_true(queued <= CONFIG_CAN_VIRTIO_TX_SLOTS, "more in flight than slots");
	}
	zassert_equal(queued, CONFIG_CAN_VIRTIO_TX_SLOTS);
	zassert_equal(can_send(can, &f, K_NO_WAIT, tx_cb, NULL), -EAGAIN);
	/* One completes: one slot frees. */
	zassert_true(complete_tx(0) >= 0);
	zassert_ok(k_sem_take(&txcb_sem, K_MSEC(100)));
	zassert_ok(can_send(can, &f, K_NO_WAIT, tx_cb, NULL));
	/* Stop: the device fails what is pending ("marks them as used with NOT_OK"). */
	tx_auto = true;
	txcb_calls = 0;
	zassert_ok(can_stop(can));
	while (complete_tx(1) >= 0) {
	}
	zassert_equal(txcb_calls, CONFIG_CAN_VIRTIO_TX_SLOTS);
	zassert_equal(txcb_err, -ENETDOWN);
}

ZTEST(can_virtio, test_rx_filters)
{
	const struct can_filter std = {.id = 0x321, .mask = CAN_STD_ID_MASK};
	const struct can_filter ext = {.id = 0x18DAF110, .mask = CAN_EXT_ID_MASK,
				       .flags = CAN_FILTER_IDE};
	const struct can_filter masked = {.id = 0x7E0, .mask = 0x7F0};
	const uint8_t d4[] = {0xDE, 0xAD, 0xBE, 0xEF};
	struct can_frame f;
	int a = can_add_rx_filter_msgq(can, &rxq, &std);
	int b = can_add_rx_filter_msgq(can, &rxq, &ext);
	int c = can_add_rx_filter_msgq(can, &rxq, &masked);

	zassert_true(a >= 0 && b >= 0 && c >= 0);
	/* Stopped: frames are dropped, buffers still come back. */
	uint16_t avail = fake_virtio_avail_count(vdev, QRX);

	inject_rx(0, 0x321, d4, 4);
	zassert_not_equal(k_msgq_get(&rxq, &f, K_MSEC(10)), 0, "delivered while stopped");
	zassert_equal(fake_virtio_avail_count(vdev, QRX), avail);

	start();
	inject_rx(0, 0x321, d4, 4);
	zassert_ok(k_msgq_get(&rxq, &f, K_MSEC(10)));
	zassert_equal(f.id, 0x321);
	zassert_equal(f.flags, 0);
	zassert_equal(f.dlc, 4);
	zassert_mem_equal(f.data, d4, 4);

	inject_rx(F_EXT, 0x18DAF110, d4, 2);
	zassert_ok(k_msgq_get(&rxq, &f, K_MSEC(10)));
	zassert_equal(f.flags, CAN_FRAME_IDE);
	zassert_equal(f.id, 0x18DAF110);

	inject_rx(0, 0x7E5, d4, 1); /* matches 0x7E0/0x7F0 */
	zassert_ok(k_msgq_get(&rxq, &f, K_MSEC(10)));
	zassert_equal(f.id, 0x7E5);

	inject_rx(0, 0x555, d4, 1);         /* no filter */
	inject_rx(F_EXT, 0x321, d4, 1);     /* ext frame, std filter */
	zassert_not_equal(k_msgq_get(&rxq, &f, K_MSEC(10)), 0, "unfiltered frame delivered");

	can_remove_rx_filter(can, a);
	inject_rx(0, 0x321, d4, 4);
	zassert_not_equal(k_msgq_get(&rxq, &f, K_MSEC(10)), 0, "removed filter still matches");
	can_remove_rx_filter(can, b);
	can_remove_rx_filter(can, c);
	zassert_equal(fake_virtio_avail_count(vdev, QRX), avail, "RX buffers leaked");
}

ZTEST(can_virtio, test_rx_malformed_dropped_and_requeued)
{
	const struct can_filter all = {.id = 0, .mask = 0};
	const uint8_t d[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	struct can_frame f;
	int id = can_add_rx_filter_msgq(can, &rxq, &all);

	start();
	uint16_t avail = fake_virtio_avail_count(vdev, QRX);

	inject_rx_raw(0x0001, 0, 0x100, d, 1, -1);          /* wrong msg_type */
	inject_rx_raw(MSG_RX, 0x0001, 0x100, d, 1, -1);     /* undefined flag bit */
	inject_rx_raw(MSG_RX, 0, 0x100, d, 1, 10);          /* shorter than the header */
	inject_rx_raw(MSG_RX, 0, 0x100, d, 8, 16 + 4);      /* sdu claims 8, buffer has 4 */
	inject_rx_raw(MSG_RX, 0, 0x100, d, 9, -1);          /* classic length > 8 */
	if (!IS_ENABLED(CONFIG_CAN_FD_MODE)) {
		inject_rx(F_FD, 0x100, d, 8);               /* FD not negotiated */
	}
	zassert_not_equal(k_msgq_get(&rxq, &f, K_MSEC(10)), 0, "malformed frame delivered");
	zassert_equal(fake_virtio_avail_count(vdev, QRX), avail, "RX buffers leaked");
	/* RTR arrives as RTR. */
	inject_rx(F_RTR, 0x100, d, 3);
	if (IS_ENABLED(CONFIG_CAN_ACCEPT_RTR)) {
		zassert_ok(k_msgq_get(&rxq, &f, K_MSEC(10)));
		zassert_equal(f.flags, CAN_FRAME_RTR);
		zassert_equal(f.dlc, 3);
	} else {
		zassert_not_equal(k_msgq_get(&rxq, &f, K_MSEC(10)), 0);
	}
	can_remove_rx_filter(can, id);
}

ZTEST(can_virtio, test_bus_off)
{
	struct can_frame f = {.id = 0x30, .dlc = 1};
	enum can_state st;

	state_cb_register();
	start();
	state_calls = 0;
	fake_virtio_config_status = sys_cpu_to_le16(1); /* VIRTIO_CAN_S_CTRL_BUSOFF */
	tx_result = 1;
	zassert_ok(can_send(can, &f, K_MSEC(100), tx_cb, NULL));
	zassert_ok(k_sem_take(&txcb_sem, K_MSEC(100)));
	zassert_equal(txcb_err, -ENETUNREACH);
	zassert_equal(state_calls, 1);
	zassert_equal(last_state, CAN_STATE_BUS_OFF);
#ifdef STATE_CB_LIST
	zassert_equal(state_calls_b, 1, "every registered callback must fire");
#endif
	zassert_ok(can_get_state(can, &st, NULL));
	zassert_equal(st, CAN_STATE_BUS_OFF);
	zassert_equal(can_send(can, &f, K_NO_WAIT, NULL, NULL), -ENETDOWN, "stopped after bus-off");
	state_cb_unregister();
	tx_result = 0;
	fake_virtio_config_status = 0;
}

#ifdef CONFIG_CAN_FD_MODE
ZTEST(can_virtio, test_fd)
{
	const struct can_filter all = {.id = 0, .mask = 0};
	struct can_frame f = {.id = 0x123, .flags = CAN_FRAME_FDF | CAN_FRAME_BRS,
			      .dlc = can_bytes_to_dlc(64)};
	uint8_t d[64];
	can_mode_t cap;
	int i;

	for (int k = 0; k < 64; k++) {
		f.data[k] = d[k] = k;
	}
	zassert_ok(can_get_capabilities(can, &cap));
	zassert_true(cap & CAN_MODE_FD);
	/* Frame type is checked before state, as in Zephyr's can_loopback. */
	zassert_equal(can_send(can, &f, K_NO_WAIT, NULL, NULL), -ENOTSUP, "FD frame in classic mode");
	start();
	zassert_equal(can_send(can, &f, K_NO_WAIT, NULL, NULL), -ENOTSUP, "FD frame in classic mode");
	zassert_ok(can_stop(can));
	zassert_ok(can_set_mode(can, CAN_MODE_FD));
	start();
	int base = tx_seen;

	zassert_ok(can_send(can, &f, K_MSEC(100), NULL, NULL));
	i = base % ARRAY_SIZE(tx_log);
	zassert_equal(sys_le32_to_cpu(tx_log[i].h.flags), F_FD);
	zassert_equal(sys_le16_to_cpu(tx_log[i].h.length), 64);
	zassert_mem_equal(tx_log[i].sdu, d, 64);
	f.flags |= CAN_FRAME_RTR;
	zassert_equal(can_send(can, &f, K_NO_WAIT, NULL, NULL), -ENOTSUP, "RTR is classic only");

	int id = can_add_rx_filter_msgq(can, &rxq, &all);

	inject_rx(F_FD, 0x234, d, 48);
	zassert_ok(k_msgq_get(&rxq, &f, K_MSEC(10)));
	zassert_equal(f.flags, CAN_FRAME_FDF);
	zassert_equal(can_dlc_to_bytes(f.dlc), 48);
	zassert_mem_equal(f.data, d, 48);
	can_remove_rx_filter(can, id);
}
#endif

ZTEST_SUITE(can_virtio, NULL, suite_setup, reset_fixture, NULL, NULL);
