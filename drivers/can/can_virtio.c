/*
 * Copyright (c) 2026 Xaloqi
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * VIRTIO CAN device driver (device ID 36).
 *
 * Written from the OASIS VIRTIO 1.4 specification, section "CAN Device"
 * (oasis-tcs/virtio-spec, branch virtio-1.4, device-types/can/description.tex
 * at 6c2340547951). Section references below are to that text.
 *
 * Three virtqueues: 0 Txq, 1 Rxq, 2 Controlq. The CAN controller is owned by
 * the device side; this driver has no bitrate control (the specification
 * defines none), so set_timing() is accepted and ignored.
 *
 * Virtqueue callbacks run in the virtio transport's ISR.
 */

#define DT_DRV_COMPAT virtio_device24

#include <string.h>

#include <zephyr/drivers/can.h>
#include <zephyr/drivers/virtio.h>
#include <zephyr/drivers/virtio/virtqueue.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(can_virtio, CONFIG_CAN_LOG_LEVEL);

/* Virtqueues ("Virtqueues") */
#define VIRTIO_CAN_QUEUE_TX   0
#define VIRTIO_CAN_QUEUE_RX   1
#define VIRTIO_CAN_QUEUE_CTRL 2
#define VIRTIO_CAN_QUEUE_NUM  3

/* Feature bits ("Feature bits") */
#define VIRTIO_CAN_F_CAN_CLASSIC 0
#define VIRTIO_CAN_F_CAN_FD      1
#define VIRTIO_CAN_F_RTR_FRAMES  2
#define VIRTIO_CAN_F_LATE_TX_ACK 3

/* Device configuration layout: le16 status */
#define VIRTIO_CAN_S_CTRL_BUSOFF BIT(0)

/* Device Operation */
#define VIRTIO_CAN_RESULT_OK 0

#define VIRTIO_CAN_SET_CTRL_MODE_START 0x0201
#define VIRTIO_CAN_SET_CTRL_MODE_STOP  0x0202

#define VIRTIO_CAN_TX 0x0001
#define VIRTIO_CAN_RX 0x0101

#define VIRTIO_CAN_FLAGS_RTR      0x2000
#define VIRTIO_CAN_FLAGS_FD       0x4000
#define VIRTIO_CAN_FLAGS_EXTENDED 0x8000

#ifdef CONFIG_CAN_FD_MODE
#define CAN_VIRTIO_MAX_SDU 64
#else
#define CAN_VIRTIO_MAX_SDU 8
#endif

/*
 * The common header of struct virtio_can_tx_out and struct virtio_can_rx.
 * All multi-byte fields are little-endian.
 */
struct virtio_can_hdr {
	uint16_t msg_type;
	uint16_t length;
	uint8_t reserved_classic_dlc;
	uint8_t padding;
	uint16_t reserved_xl_priority;
	uint32_t flags;
	uint32_t can_id;
};

BUILD_ASSERT(sizeof(struct virtio_can_hdr) == 16, "virtio_can header must be 16 bytes");

struct virtio_can_frame_buf {
	struct virtio_can_hdr hdr;
	uint8_t sdu[CAN_VIRTIO_MAX_SDU];
};

struct can_virtio_tx_slot {
	struct virtio_can_frame_buf out; /* device-readable */
	uint8_t result;                  /* device-writable */
	const struct device *dev;
	can_tx_callback_t cb;
	void *user_data;
};

/*
 * RX buffers always have room for a 64-byte sdu. The specification only needs
 * the negotiated maximum (8 bytes for classic CAN), but rust-vmm's
 * vhost-device-can writes the full 64-byte sdu into every RX buffer and stops
 * servicing its queues when the buffer is shorter. Linux sizes them the same way.
 */
#define CAN_VIRTIO_RX_SDU 64

struct can_virtio_rx_buf {
	struct {
		struct virtio_can_hdr hdr;
		uint8_t sdu[CAN_VIRTIO_RX_SDU];
	} in; /* device-writable */
	const struct device *dev;
};

struct can_virtio_filter {
	can_rx_callback_t cb;
	void *user_data;
	struct can_filter filter;
};

struct can_virtio_config {
	const struct can_driver_config common;
	const struct device *vdev;
};

#define TX_QUEUE_SIZE  (1U << LOG2CEIL(2 * CONFIG_CAN_VIRTIO_TX_SLOTS))
#define RX_QUEUE_SIZE  (1U << LOG2CEIL(CONFIG_CAN_VIRTIO_RX_BUFFERS))
#define CTRL_QUEUE_SIZE 2U

struct can_virtio_data {
	struct can_driver_data common;

	bool feat_fd;
	bool feat_rtr;
	uint16_t tx_slots_n;
	uint16_t rx_bufs_n;

	struct can_virtio_tx_slot tx_slots[CONFIG_CAN_VIRTIO_TX_SLOTS];
	atomic_t tx_busy[ATOMIC_BITMAP_SIZE(CONFIG_CAN_VIRTIO_TX_SLOTS)];
	struct k_sem tx_sem;
	struct k_spinlock tx_lock;

	struct can_virtio_rx_buf rx_bufs[RX_QUEUE_SIZE];

	struct can_virtio_filter filters[CONFIG_CAN_VIRTIO_MAX_FILTERS];
	struct k_spinlock filter_lock;

	/* One control request at a time ("Control queue messages are processed in order") */
	struct k_mutex ctrl_mtx;
	struct k_sem ctrl_done;
	uint16_t ctrl_out; /* le16 msg_type, device-readable */
	uint8_t ctrl_in;   /* u8 result, device-writable */
	bool ctrl_inflight; /* a timed-out request still owns ctrl_out/ctrl_in */
};

static uint16_t can_virtio_read_status(const struct device *dev)
{
	const struct can_virtio_config *cfg = dev->config;
	mem_addr_t reg = (mem_addr_t)virtio_get_device_specific_config(cfg->vdev);

	if (reg == 0) {
		return 0;
	}

	return sys_le16_to_cpu(sys_read16(reg));
}

/*
 * Bus-off is reported through the configuration status field. Zephyr's
 * virtio transports do not deliver configuration change interrupts yet, so
 * the field is read when a transmission fails and in get_state().
 */
static void can_virtio_check_bus_off(const struct device *dev)
{
	struct can_virtio_data *data = dev->data;
	struct can_bus_err_cnt err_cnt = {0};
	can_state_change_callback_t cb = data->common.state_change_cb;

	if ((can_virtio_read_status(dev) & VIRTIO_CAN_S_CTRL_BUSOFF) == 0U) {
		return;
	}

	if (!data->common.started) {
		return;
	}

	/* "After bus-off detection the CAN controller is in STOPPED state." */
	data->common.started = false;
	LOG_WRN("device reports bus-off");

	if (cb != NULL) {
		cb(dev, CAN_STATE_BUS_OFF, err_cnt, data->common.state_change_cb_user_data);
	}
}

/* --- Transmission ------------------------------------------------------ */

static void can_virtio_tx_done(void *opaque, uint32_t used_len)
{
	struct can_virtio_tx_slot *slot = opaque;
	const struct device *dev = slot->dev;
	struct can_virtio_data *data = dev->data;
	can_tx_callback_t cb = slot->cb;
	void *user_data = slot->user_data;
	int err = 0;

	if (used_len < 1U || slot->result != VIRTIO_CAN_RESULT_OK) {
		if (!data->common.started) {
			err = -ENETDOWN;
		} else {
			err = -EIO;
		}
	}

	atomic_clear_bit(data->tx_busy, slot - data->tx_slots);
	k_sem_give(&data->tx_sem);

	if (err == -EIO) {
		can_virtio_check_bus_off(dev);
		if (!data->common.started) {
			err = -ENETUNREACH;
		}
	}

	if (cb != NULL) {
		cb(dev, err, user_data);
	}
}

static int can_virtio_send(const struct device *dev, const struct can_frame *frame,
			   k_timeout_t timeout, can_tx_callback_t callback, void *user_data)
{
	const struct can_virtio_config *cfg = dev->config;
	struct can_virtio_data *data = dev->data;
	struct virtq *vq = virtio_get_virtqueue(cfg->vdev, VIRTIO_CAN_QUEUE_TX);
	struct can_virtio_tx_slot *slot = NULL;
	uint8_t max_dlc = CAN_MAX_DLC;
	uint32_t flags = 0U;
	uint16_t length;
	k_spinlock_key_t key;
	int ret;

#ifdef CONFIG_CAN_FD_MODE
	if ((frame->flags & ~(CAN_FRAME_IDE | CAN_FRAME_RTR | CAN_FRAME_FDF | CAN_FRAME_BRS)) !=
	    0U) {
		LOG_ERR("unsupported CAN frame flags 0x%02x", frame->flags);
		return -ENOTSUP;
	}

	if ((frame->flags & CAN_FRAME_FDF) != 0U) {
		if ((data->common.mode & CAN_MODE_FD) == 0U) {
			return -ENOTSUP;
		}
		max_dlc = CANFD_MAX_DLC;
		flags |= VIRTIO_CAN_FLAGS_FD;
	}
#else
	if ((frame->flags & ~(CAN_FRAME_IDE | CAN_FRAME_RTR)) != 0U) {
		LOG_ERR("unsupported CAN frame flags 0x%02x", frame->flags);
		return -ENOTSUP;
	}
#endif

	if ((frame->flags & CAN_FRAME_RTR) != 0U) {
		/* "RTR frames are only supported with classic CAN." */
		if (!data->feat_rtr || (flags & VIRTIO_CAN_FLAGS_FD) != 0U) {
			return -ENOTSUP;
		}
		flags |= VIRTIO_CAN_FLAGS_RTR;
	}

	if ((frame->flags & CAN_FRAME_IDE) != 0U) {
		flags |= VIRTIO_CAN_FLAGS_EXTENDED;
	}

	if (frame->dlc > max_dlc) {
		LOG_ERR("DLC of %d exceeds maximum (%d)", frame->dlc, max_dlc);
		return -EINVAL;
	}

	if (!data->common.started) {
		return -ENETDOWN;
	}

	if (vq == NULL) {
		return -EIO;
	}

	if (k_sem_take(&data->tx_sem, timeout) != 0) {
		return -EAGAIN;
	}

	for (uint16_t i = 0; i < data->tx_slots_n; i++) {
		if (!atomic_test_and_set_bit(data->tx_busy, i)) {
			slot = &data->tx_slots[i];
			break;
		}
	}

	if (slot == NULL) {
		/* Cannot happen: the semaphore counts free slots. */
		k_sem_give(&data->tx_sem);
		return -EAGAIN;
	}

	length = can_dlc_to_bytes(frame->dlc);

	slot->out.hdr.msg_type = sys_cpu_to_le16(VIRTIO_CAN_TX);
	slot->out.hdr.length = sys_cpu_to_le16(length);
	slot->out.hdr.reserved_classic_dlc = 0U;
	slot->out.hdr.padding = 0U;
	slot->out.hdr.reserved_xl_priority = 0U;
	slot->out.hdr.flags = sys_cpu_to_le32(flags);
	slot->out.hdr.can_id = sys_cpu_to_le32(frame->id);
	if ((flags & VIRTIO_CAN_FLAGS_RTR) == 0U) {
		memcpy(slot->out.sdu, frame->data, length);
	} else {
		/* An RTR frame carries the requested length but no payload. */
		memset(slot->out.sdu, 0, length);
	}
	slot->result = 0xffU;
	slot->cb = callback;
	slot->user_data = user_data;

	struct virtq_buf bufs[] = {
		{.addr = &slot->out, .len = sizeof(slot->out.hdr) + length},
		{.addr = &slot->result, .len = sizeof(slot->result)},
	};

	key = k_spin_lock(&data->tx_lock);
	ret = virtq_add_buffer_chain(vq, bufs, ARRAY_SIZE(bufs), 1, can_virtio_tx_done, slot,
				     K_NO_WAIT);
	if (ret == 0) {
		virtio_notify_virtqueue(cfg->vdev, VIRTIO_CAN_QUEUE_TX);
	}
	k_spin_unlock(&data->tx_lock, key);

	if (ret != 0) {
		LOG_ERR("TX virtq_add_buffer_chain failed: %d", ret);
		atomic_clear_bit(data->tx_busy, slot - data->tx_slots);
		k_sem_give(&data->tx_sem);
		return -EIO;
	}

	return 0;
}

/* --- Reception --------------------------------------------------------- */

static void can_virtio_rx_done(void *opaque, uint32_t used_len);

static int can_virtio_rx_queue_buf(const struct device *dev, struct can_virtio_rx_buf *buf)
{
	const struct can_virtio_config *cfg = dev->config;
	struct virtq *vq = virtio_get_virtqueue(cfg->vdev, VIRTIO_CAN_QUEUE_RX);
	struct virtq_buf vbuf = {.addr = &buf->in, .len = sizeof(buf->in)};

	if (vq == NULL) {
		return -EIO;
	}

	return virtq_add_buffer_chain(vq, &vbuf, 1, 0, can_virtio_rx_done, buf, K_NO_WAIT);
}

static bool can_virtio_rx_to_frame(struct can_virtio_data *data,
				   const struct can_virtio_rx_buf *buf, uint32_t used_len,
				   struct can_frame *frame)
{
	const __typeof__(buf->in) *in = &buf->in;
	uint32_t flags = sys_le32_to_cpu(in->hdr.flags);
	uint32_t can_id = sys_le32_to_cpu(in->hdr.can_id);
	uint16_t length = sys_le16_to_cpu(in->hdr.length);
	uint16_t max_len = 8U; /* classic CAN, whatever CAN_MAX_DLEN is with CONFIG_CAN_FD_MODE */

	if (used_len < sizeof(in->hdr) || sys_le16_to_cpu(in->hdr.msg_type) != VIRTIO_CAN_RX) {
		LOG_WRN("RX: malformed buffer (len %u, type 0x%04x)", used_len,
			sys_le16_to_cpu(in->hdr.msg_type));
		return false;
	}

	if ((flags & ~(VIRTIO_CAN_FLAGS_RTR | VIRTIO_CAN_FLAGS_FD | VIRTIO_CAN_FLAGS_EXTENDED)) !=
	    0U) {
		LOG_WRN("RX: undefined flags 0x%08x", flags);
		return false;
	}

	memset(frame, 0, sizeof(*frame));

	if ((flags & VIRTIO_CAN_FLAGS_FD) != 0U) {
#ifdef CONFIG_CAN_FD_MODE
		if (!data->feat_fd || (data->common.mode & CAN_MODE_FD) == 0U) {
			return false;
		}
		frame->flags |= CAN_FRAME_FDF;
		max_len = 64U;
#else
		return false;
#endif
	}

	if ((flags & VIRTIO_CAN_FLAGS_RTR) != 0U) {
#ifndef CONFIG_CAN_ACCEPT_RTR
		return false;
#else
		frame->flags |= CAN_FRAME_RTR;
#endif
	}

	if ((flags & VIRTIO_CAN_FLAGS_EXTENDED) != 0U) {
		frame->flags |= CAN_FRAME_IDE;
		frame->id = can_id & CAN_EXT_ID_MASK;
	} else {
		frame->id = can_id & CAN_STD_ID_MASK;
	}

	if (length > max_len) {
		LOG_WRN("RX: length %u exceeds %u", length, max_len);
		return false;
	}

	if ((frame->flags & CAN_FRAME_RTR) == 0U) {
		if (used_len < sizeof(in->hdr) + length) {
			LOG_WRN("RX: short buffer (%u < %u)", used_len,
				(unsigned int)(sizeof(in->hdr) + length));
			return false;
		}
		memcpy(frame->data, in->sdu, length);
	}

	frame->dlc = can_bytes_to_dlc(length);

	return true;
}

static void can_virtio_rx_done(void *opaque, uint32_t used_len)
{
	struct can_virtio_rx_buf *buf = opaque;
	const struct device *dev = buf->dev;
	const struct can_virtio_config *cfg = dev->config;
	struct can_virtio_data *data = dev->data;
	struct can_frame frame;
	int ret;

	if (data->common.started && can_virtio_rx_to_frame(data, buf, used_len, &frame)) {
		for (int i = 0; i < ARRAY_SIZE(data->filters); i++) {
			struct can_virtio_filter f;
			k_spinlock_key_t key = k_spin_lock(&data->filter_lock);

			f = data->filters[i];
			k_spin_unlock(&data->filter_lock, key);

			if (f.cb != NULL && can_frame_matches_filter(&frame, &f.filter)) {
				struct can_frame tmp = frame;

				f.cb(dev, &tmp, f.user_data);
			}
		}
	}

	/* Give the buffer back to the device. */
	ret = can_virtio_rx_queue_buf(dev, buf);
	if (ret != 0) {
		LOG_ERR("RX: re-queue failed: %d", ret);
		return;
	}
	virtio_notify_virtqueue(cfg->vdev, VIRTIO_CAN_QUEUE_RX);
}

/* --- Filters ----------------------------------------------------------- */

static int can_virtio_add_rx_filter(const struct device *dev, can_rx_callback_t cb,
				    void *user_data, const struct can_filter *filter)
{
	struct can_virtio_data *data = dev->data;
	k_spinlock_key_t key;
	int filter_id = -ENOSPC;

	if ((filter->flags & ~(CAN_FILTER_IDE)) != 0U) {
		LOG_ERR("unsupported CAN filter flags 0x%02x", filter->flags);
		return -ENOTSUP;
	}

	key = k_spin_lock(&data->filter_lock);
	for (int i = 0; i < ARRAY_SIZE(data->filters); i++) {
		if (data->filters[i].cb == NULL) {
			data->filters[i].filter = *filter;
			data->filters[i].user_data = user_data;
			data->filters[i].cb = cb;
			filter_id = i;
			break;
		}
	}
	k_spin_unlock(&data->filter_lock, key);

	if (filter_id < 0) {
		LOG_ERR("no free filter left");
	}

	return filter_id;
}

static void can_virtio_remove_rx_filter(const struct device *dev, int filter_id)
{
	struct can_virtio_data *data = dev->data;
	k_spinlock_key_t key;

	if (filter_id < 0 || filter_id >= ARRAY_SIZE(data->filters)) {
		LOG_ERR("filter ID %d out of bounds", filter_id);
		return;
	}

	key = k_spin_lock(&data->filter_lock);
	data->filters[filter_id].cb = NULL;
	k_spin_unlock(&data->filter_lock, key);
}

static int can_virtio_get_max_filters(const struct device *dev, bool ide)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ide);

	return CONFIG_CAN_VIRTIO_MAX_FILTERS;
}

/* --- Controller mode (control queue) ----------------------------------- */

static void can_virtio_ctrl_done(void *opaque, uint32_t used_len)
{
	struct can_virtio_data *data = opaque;

	if (used_len < 1U) {
		data->ctrl_in = 0xffU;
	}
	k_sem_give(&data->ctrl_done);
}

static int can_virtio_ctrl(const struct device *dev, uint16_t msg_type)
{
	const struct can_virtio_config *cfg = dev->config;
	struct can_virtio_data *data = dev->data;
	struct virtq *vq = virtio_get_virtqueue(cfg->vdev, VIRTIO_CAN_QUEUE_CTRL);
	int ret;

	if (vq == NULL) {
		return -EIO;
	}

	k_mutex_lock(&data->ctrl_mtx, K_FOREVER);

	/*
	 * A request that timed out is still on the queue and still owns the
	 * buffers. Its completion gives ctrl_done; wait for it once more before
	 * reusing them, and refuse if the device still does not answer.
	 */
	if (data->ctrl_inflight) {
		if (k_sem_take(&data->ctrl_done, K_MSEC(CONFIG_CAN_VIRTIO_CTRL_TIMEOUT_MS)) != 0) {
			LOG_ERR("control queue: an earlier request is still unanswered");
			ret = -EIO;
			goto out;
		}
		data->ctrl_inflight = false;
	}

	struct virtq_buf bufs[] = {
		{.addr = &data->ctrl_out, .len = sizeof(data->ctrl_out)},
		{.addr = &data->ctrl_in, .len = sizeof(data->ctrl_in)},
	};

	data->ctrl_out = sys_cpu_to_le16(msg_type);
	data->ctrl_in = 0xffU;
	k_sem_reset(&data->ctrl_done);

	ret = virtq_add_buffer_chain(vq, bufs, ARRAY_SIZE(bufs), 1, can_virtio_ctrl_done, data,
				     K_NO_WAIT);
	if (ret != 0) {
		LOG_ERR("control virtq_add_buffer_chain failed: %d", ret);
		ret = -EIO;
		goto out;
	}
	virtio_notify_virtqueue(cfg->vdev, VIRTIO_CAN_QUEUE_CTRL);

	if (k_sem_take(&data->ctrl_done, K_MSEC(CONFIG_CAN_VIRTIO_CTRL_TIMEOUT_MS)) != 0) {
		/* The device is not answering; the request stays queued. */
		LOG_ERR("control request 0x%04x: no answer from the device", msg_type);
		data->ctrl_inflight = true;
		ret = -EIO;
		goto out;
	}

	ret = (data->ctrl_in == VIRTIO_CAN_RESULT_OK) ? 0 : -EIO;
	if (ret != 0) {
		LOG_ERR("control request 0x%04x: result %u", msg_type, data->ctrl_in);
	}

out:
	k_mutex_unlock(&data->ctrl_mtx);
	return ret;
}

static int can_virtio_start(const struct device *dev)
{
	struct can_virtio_data *data = dev->data;
	int ret;

	if (data->common.started) {
		return -EALREADY;
	}

	ret = can_virtio_ctrl(dev, VIRTIO_CAN_SET_CTRL_MODE_START);
	if (ret != 0) {
		return ret;
	}

	data->common.started = true;

	return 0;
}

static int can_virtio_stop(const struct device *dev)
{
	struct can_virtio_data *data = dev->data;
	int ret;

	if (!data->common.started) {
		return -EALREADY;
	}

	/*
	 * Set before the request: the device fails every pending transmission
	 * on the way to STOPPED, and those completions must report -ENETDOWN.
	 */
	data->common.started = false;

	ret = can_virtio_ctrl(dev, VIRTIO_CAN_SET_CTRL_MODE_STOP);
	if (ret != 0) {
		LOG_WRN("stop request failed (%d); the driver is stopped regardless", ret);
	}

	return ret;
}

static int can_virtio_get_capabilities(const struct device *dev, can_mode_t *cap)
{
	struct can_virtio_data *data = dev->data;

	*cap = CAN_MODE_NORMAL;
#ifdef CONFIG_CAN_FD_MODE
	if (data->feat_fd) {
		*cap |= CAN_MODE_FD;
	}
#else
	ARG_UNUSED(data);
#endif

	return 0;
}

static int can_virtio_set_mode(const struct device *dev, can_mode_t mode)
{
	struct can_virtio_data *data = dev->data;
	can_mode_t cap;

	if (data->common.started) {
		return -EBUSY;
	}

	(void)can_virtio_get_capabilities(dev, &cap);
	if ((mode & ~cap) != 0U) {
		LOG_ERR("unsupported mode: 0x%08x", mode);
		return -ENOTSUP;
	}

	data->common.mode = mode;

	return 0;
}

/*
 * The VIRTIO CAN device has no bit timing configuration: the controller and
 * its bitrate belong to the device side. Timing is accepted (so that
 * can_set_bitrate() and a devicetree bitrate work as on any controller) and
 * not forwarded.
 */
static int can_virtio_set_timing(const struct device *dev, const struct can_timing *timing)
{
	struct can_virtio_data *data = dev->data;

	ARG_UNUSED(timing);

	if (data->common.started) {
		return -EBUSY;
	}

	LOG_DBG("bit timing is owned by the device side; ignored");

	return 0;
}

#ifdef CONFIG_CAN_FD_MODE
static int can_virtio_set_timing_data(const struct device *dev, const struct can_timing *timing)
{
	return can_virtio_set_timing(dev, timing);
}
#endif

static int can_virtio_get_state(const struct device *dev, enum can_state *state,
				struct can_bus_err_cnt *err_cnt)
{
	struct can_virtio_data *data = dev->data;

	if (state != NULL) {
		if ((can_virtio_read_status(dev) & VIRTIO_CAN_S_CTRL_BUSOFF) != 0U) {
			*state = CAN_STATE_BUS_OFF;
		} else if (data->common.started) {
			/* The specification reports no error-passive/warning states. */
			*state = CAN_STATE_ERROR_ACTIVE;
		} else {
			*state = CAN_STATE_STOPPED;
		}
	}

	if (err_cnt != NULL) {
		/* Not exposed by the device. */
		err_cnt->tx_err_cnt = 0U;
		err_cnt->rx_err_cnt = 0U;
	}

	return 0;
}

static void can_virtio_set_state_change_callback(const struct device *dev,
						 can_state_change_callback_t cb, void *user_data)
{
	struct can_virtio_data *data = dev->data;

	data->common.state_change_cb = cb;
	data->common.state_change_cb_user_data = user_data;
}

static int can_virtio_get_core_clock(const struct device *dev, uint32_t *rate)
{
	ARG_UNUSED(dev);

	/* Nominal value for the timing calculator only (CiA 601-3 recommendation). */
	*rate = MHZ(80);

	return 0;
}

static DEVICE_API(can, can_virtio_driver_api) = {
	.get_capabilities = can_virtio_get_capabilities,
	.start = can_virtio_start,
	.stop = can_virtio_stop,
	.set_mode = can_virtio_set_mode,
	.set_timing = can_virtio_set_timing,
	.send = can_virtio_send,
	.add_rx_filter = can_virtio_add_rx_filter,
	.remove_rx_filter = can_virtio_remove_rx_filter,
	.get_state = can_virtio_get_state,
	.set_state_change_callback = can_virtio_set_state_change_callback,
	.get_core_clock = can_virtio_get_core_clock,
	.get_max_filters = can_virtio_get_max_filters,
	/* Recommended configuration ranges from CiA 601-2 */
	.timing_min = {.sjw = 1, .prop_seg = 0, .phase_seg1 = 2, .phase_seg2 = 2, .prescaler = 1},
	.timing_max = {.sjw = 128, .prop_seg = 0, .phase_seg1 = 256, .phase_seg2 = 128,
		       .prescaler = 32},
#ifdef CONFIG_CAN_FD_MODE
	.set_timing_data = can_virtio_set_timing_data,
	.timing_data_min = {.sjw = 1, .prop_seg = 0, .phase_seg1 = 1, .phase_seg2 = 1,
			    .prescaler = 1},
	.timing_data_max = {.sjw = 16, .prop_seg = 0, .phase_seg1 = 32, .phase_seg2 = 16,
			    .prescaler = 32},
#endif
};

/* --- Initialisation ---------------------------------------------------- */

static uint16_t can_virtio_enum_queues(uint16_t q_index, uint16_t q_size_max, void *opaque)
{
	struct can_virtio_data *data = opaque;
	uint16_t want;
	uint16_t size;

	switch (q_index) {
	case VIRTIO_CAN_QUEUE_TX:
		want = TX_QUEUE_SIZE;
		break;
	case VIRTIO_CAN_QUEUE_RX:
		want = RX_QUEUE_SIZE;
		break;
	case VIRTIO_CAN_QUEUE_CTRL:
		want = CTRL_QUEUE_SIZE;
		break;
	default:
		return 0;
	}

	/* virtq_create() needs a power of two. */
	size = MIN(want, q_size_max);
	while ((size & (size - 1U)) != 0U) {
		size &= size - 1U;
	}

	if (q_index == VIRTIO_CAN_QUEUE_TX) {
		data->tx_slots_n = MIN(CONFIG_CAN_VIRTIO_TX_SLOTS, size / 2U);
	} else if (q_index == VIRTIO_CAN_QUEUE_RX) {
		data->rx_bufs_n = size;
	}

	return size;
}

static int can_virtio_init(const struct device *dev)
{
	const struct can_virtio_config *cfg = dev->config;
	struct can_virtio_data *data = dev->data;
	bool classic;
	int ret;

	if (!device_is_ready(cfg->vdev)) {
		LOG_ERR("virtio transport %s not ready", cfg->vdev->name);
		return -ENODEV;
	}

	k_mutex_init(&data->ctrl_mtx);
	k_sem_init(&data->ctrl_done, 0, 1);

	/* Feature negotiation ("Feature bit requirements") */
	classic = virtio_read_device_feature_bit(cfg->vdev, VIRTIO_CAN_F_CAN_CLASSIC);
#ifdef CONFIG_CAN_FD_MODE
	data->feat_fd = virtio_read_device_feature_bit(cfg->vdev, VIRTIO_CAN_F_CAN_FD);
#endif
	data->feat_rtr = classic &&
			 virtio_read_device_feature_bit(cfg->vdev, VIRTIO_CAN_F_RTR_FRAMES);

	if (!classic && !data->feat_fd) {
		LOG_ERR("device offers neither classic CAN nor CAN FD");
		return -ENODEV;
	}
	if (!classic) {
		/* Classic frames would be rejected; this driver needs them. */
		LOG_ERR("device offers no classic CAN");
		return -ENOTSUP;
	}

	ret = virtio_write_driver_feature_bit(cfg->vdev, VIRTIO_CAN_F_CAN_CLASSIC, true);
	if (ret == 0 && data->feat_fd) {
		ret = virtio_write_driver_feature_bit(cfg->vdev, VIRTIO_CAN_F_CAN_FD, true);
	}
	if (ret == 0 && data->feat_rtr) {
		ret = virtio_write_driver_feature_bit(cfg->vdev, VIRTIO_CAN_F_RTR_FRAMES, true);
	}
	if (ret == 0 && virtio_read_device_feature_bit(cfg->vdev, VIRTIO_CAN_F_LATE_TX_ACK)) {
		ret = virtio_write_driver_feature_bit(cfg->vdev, VIRTIO_CAN_F_LATE_TX_ACK, true);
	}
	if (ret != 0) {
		LOG_ERR("writing feature bits failed: %d", ret);
		return ret;
	}

	ret = virtio_commit_feature_bits(cfg->vdev);
	if (ret != 0) {
		LOG_ERR("device rejected the feature set: %d", ret);
		return ret;
	}

	ret = virtio_init_virtqueues(cfg->vdev, VIRTIO_CAN_QUEUE_NUM, can_virtio_enum_queues, data);
	if (ret != 0) {
		LOG_ERR("virtio_init_virtqueues failed: %d", ret);
		return ret;
	}

	if (data->tx_slots_n == 0U || data->rx_bufs_n == 0U) {
		LOG_ERR("device queues too small (tx slots %u, rx buffers %u)", data->tx_slots_n,
			data->rx_bufs_n);
		return -ENODEV;
	}
	k_sem_init(&data->tx_sem, data->tx_slots_n, data->tx_slots_n);

	for (uint16_t i = 0; i < data->tx_slots_n; i++) {
		data->tx_slots[i].dev = dev;
	}

	/* "The driver MUST populate the Rxq with empty device-writeable buffers" */
	for (uint16_t i = 0; i < data->rx_bufs_n; i++) {
		data->rx_bufs[i].dev = dev;
		ret = can_virtio_rx_queue_buf(dev, &data->rx_bufs[i]);
		if (ret != 0) {
			LOG_ERR("RX buffer %u: %d", i, ret);
			return ret;
		}
	}

	virtio_finalize_init(cfg->vdev);
	/* Notifications only after DRIVER_OK. */
	virtio_notify_virtqueue(cfg->vdev, VIRTIO_CAN_QUEUE_RX);

	LOG_INF("virtio-can: classic%s%s%s, %u TX slots, %u RX buffers",
		data->feat_fd ? " +FD" : "", data->feat_rtr ? " +RTR" : "",
		virtio_read_device_feature_bit(cfg->vdev, VIRTIO_CAN_F_LATE_TX_ACK) ? " +LATE_TX_ACK"
										   : "",
		data->tx_slots_n, data->rx_bufs_n);

	/* "Initially the CAN controller is in the STOPPED state." */
	data->common.started = false;

	return 0;
}

#ifdef CONFIG_CAN_FD_MODE
#define CAN_VIRTIO_MAX_BITRATE 8000000
#else
#define CAN_VIRTIO_MAX_BITRATE 1000000
#endif

#define CAN_VIRTIO_INIT(inst)                                                                      \
	static const struct can_virtio_config can_virtio_config_##inst = {                         \
		.common = CAN_DT_DRIVER_CONFIG_INST_GET(inst, 0, CAN_VIRTIO_MAX_BITRATE),          \
		.vdev = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                       \
	};                                                                                         \
                                                                                                   \
	static struct can_virtio_data can_virtio_data_##inst;                                      \
                                                                                                   \
	CAN_DEVICE_DT_INST_DEFINE(inst, can_virtio_init, NULL, &can_virtio_data_##inst,            \
				  &can_virtio_config_##inst, POST_KERNEL,                          \
				  CONFIG_CAN_INIT_PRIORITY, &can_virtio_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CAN_VIRTIO_INIT)
