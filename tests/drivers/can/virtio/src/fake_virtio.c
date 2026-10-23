/*
 * Copyright (c) 2026 Xaloqi
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT xaloqi_fake_virtio

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/virtio.h>
#include <zephyr/drivers/virtio/virtio_config.h>
#include <zephyr/drivers/virtio/virtqueue.h>
#include <zephyr/irq_offload.h>
#include <zephyr/sys/byteorder.h>
#include "fake_virtio.h"

/* Zephyr's own completion dispatch (drivers/virtio/virtio_common.c), as a real transport uses it. */
void virtio_isr(const struct device *dev, uint8_t isr_status, uint16_t virtqueue_count);

uint64_t fake_virtio_device_features;
uint64_t fake_virtio_driver_features;
bool fake_virtio_features_ok;
bool fake_virtio_driver_ok;
uint16_t fake_virtio_queue_size[FAKE_VIRTIO_MAX_QUEUES];
uint16_t fake_virtio_queue_max = 256;
uint32_t fake_virtio_notify_count[FAKE_VIRTIO_MAX_QUEUES];
uint32_t fake_virtio_notify_before_driver_ok;
volatile uint16_t fake_virtio_config_status;

static struct virtq queues[FAKE_VIRTIO_MAX_QUEUES];
static uint16_t queue_count;
static uint16_t last_avail[FAKE_VIRTIO_MAX_QUEUES];
static fake_virtio_notify_hook notify_hook;

void fake_virtio_set_notify_hook(fake_virtio_notify_hook hook)
{
	notify_hook = hook;
}

static struct virtq *fv_get_virtqueue(const struct device *dev, uint16_t q)
{
	return q < queue_count ? &queues[q] : NULL;
}

static void fv_notify(const struct device *dev, uint16_t q)
{
	if (!fake_virtio_driver_ok) {
		fake_virtio_notify_before_driver_ok++;
	}
	if (q < FAKE_VIRTIO_MAX_QUEUES) {
		fake_virtio_notify_count[q]++;
	}
	if (notify_hook != NULL) {
		notify_hook(dev, q);
	}
}

static void *fv_get_config(const struct device *dev)
{
	return (void *)&fake_virtio_config_status;
}

static bool fv_read_feature(const struct device *dev, int bit)
{
	return (fake_virtio_device_features & BIT64(bit)) != 0;
}

static int fv_write_feature(const struct device *dev, int bit, bool value)
{
	if (value) {
		fake_virtio_driver_features |= BIT64(bit);
	} else {
		fake_virtio_driver_features &= ~BIT64(bit);
	}
	return 0;
}

static int fv_commit(const struct device *dev)
{
	/* A device accepts only features it offered. */
	if ((fake_virtio_driver_features & ~fake_virtio_device_features) != 0) {
		return -EINVAL;
	}
	fake_virtio_features_ok = true;
	return 0;
}

static int fv_init_queues(const struct device *dev, uint16_t n, virtio_enumerate_queues cb,
			  void *opaque)
{
	if (n > FAKE_VIRTIO_MAX_QUEUES) {
		return -EINVAL;
	}
	for (uint16_t i = 0; i < n; i++) {
		uint16_t size = cb(i, fake_virtio_queue_max, opaque);
		int ret = virtq_create(&queues[i], size);

		if (ret != 0) {
			return ret;
		}
		fake_virtio_queue_size[i] = size;
		last_avail[i] = 0;
	}
	queue_count = n;
	return 0;
}

static void fv_finalize(const struct device *dev)
{
	fake_virtio_driver_ok = true;
}

uint16_t fake_virtio_avail_count(const struct device *dev, uint16_t q)
{
	return (uint16_t)(sys_le16_to_cpu(queues[q].avail->idx) - last_avail[q]);
}

bool fake_virtio_pop(const struct device *dev, uint16_t q, struct fake_chain *c)
{
	struct virtq *v = &queues[q];

	if (fake_virtio_avail_count(dev, q) == 0) {
		return false;
	}
	uint16_t idx = sys_le16_to_cpu(v->avail->ring[last_avail[q] % v->num]);

	last_avail[q]++;
	c->head = idx;
	c->n = 0;
	for (;;) {
		struct virtq_desc *d = &v->desc[idx];

		__ASSERT(c->n < FAKE_VIRTIO_MAX_CHAIN, "chain too long");
		c->addr[c->n] = (void *)(uintptr_t)d->addr;
		c->len[c->n] = d->len;
		c->writable[c->n] = (d->flags & VIRTQ_DESC_F_WRITE) != 0;
		c->n++;
		if ((d->flags & VIRTQ_DESC_F_NEXT) == 0) {
			break;
		}
		idx = d->next;
	}
	return true;
}

void fake_virtio_push_used(const struct device *dev, uint16_t q, uint16_t head, uint32_t len)
{
	struct virtq *v = &queues[q];
	uint16_t i = sys_le16_to_cpu(v->used->idx);

	v->used->ring[i % v->num].id = sys_cpu_to_le32(head);
	v->used->ring[i % v->num].len = sys_cpu_to_le32(len);
	v->used->idx = sys_cpu_to_le16(i + 1);
}

static void fv_isr(const void *arg)
{
	virtio_isr((const struct device *)arg, VIRTIO_QUEUE_INTERRUPT, queue_count);
}

void fake_virtio_interrupt(const struct device *dev)
{
	irq_offload(fv_isr, dev);
}

static DEVICE_API(virtio, fake_virtio_api) = {
	.get_virtqueue = fv_get_virtqueue,
	.notify_virtqueue = fv_notify,
	.get_device_specific_config = fv_get_config,
	.read_device_feature_bit = fv_read_feature,
	.write_driver_feature_bit = fv_write_feature,
	.commit_feature_bits = fv_commit,
	.init_virtqueues = fv_init_queues,
	.finalize_init = fv_finalize,
};

static int fake_virtio_init(const struct device *dev)
{
	/* VIRTIO_CAN_F_CAN_CLASSIC, _CAN_FD, _RTR_FRAMES, _LATE_TX_ACK, and VERSION_1. */
	fake_virtio_device_features = BIT64(0) | BIT64(1) | BIT64(2) | BIT64(3) | BIT64(32);
	fake_virtio_driver_features = BIT64(32); /* the transport writes VERSION_1 itself */
	return 0;
}

DEVICE_DT_INST_DEFINE(0, fake_virtio_init, NULL, NULL, NULL, POST_KERNEL, 0, &fake_virtio_api);
