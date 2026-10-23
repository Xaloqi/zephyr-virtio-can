/*
 * Copyright (c) 2026 Xaloqi
 * SPDX-License-Identifier: Apache-2.0
 *
 * A fake virtio transport for testing a virtio driver: the test plays the device.
 */
#ifndef FAKE_VIRTIO_H_
#define FAKE_VIRTIO_H_

#include <zephyr/kernel.h>
#include <zephyr/drivers/virtio/virtqueue.h>

#define FAKE_VIRTIO_MAX_QUEUES 3
#define FAKE_VIRTIO_MAX_CHAIN  4

struct fake_chain {
	uint16_t head;
	uint16_t n;
	void *addr[FAKE_VIRTIO_MAX_CHAIN];
	uint32_t len[FAKE_VIRTIO_MAX_CHAIN];
	bool writable[FAKE_VIRTIO_MAX_CHAIN];
};

/* Device-side configuration, read by the driver at init (set before boot by defaults). */
extern uint64_t fake_virtio_device_features;
extern uint64_t fake_virtio_driver_features;
extern bool fake_virtio_features_ok;
extern bool fake_virtio_driver_ok;
extern uint16_t fake_virtio_queue_size[FAKE_VIRTIO_MAX_QUEUES];
extern uint16_t fake_virtio_queue_max;
extern uint32_t fake_virtio_notify_count[FAKE_VIRTIO_MAX_QUEUES];
extern uint32_t fake_virtio_notify_before_driver_ok;
extern volatile uint16_t fake_virtio_config_status; /* le16 CAN status */

/* Called from the transport's notify; return true if the chain was consumed. */
typedef void (*fake_virtio_notify_hook)(const struct device *dev, uint16_t q);
void fake_virtio_set_notify_hook(fake_virtio_notify_hook hook);

/* Device side: take the next available chain of queue q. Returns false if none. */
bool fake_virtio_pop(const struct device *dev, uint16_t q, struct fake_chain *c);
/* Device side: number of chains made available and not yet popped. */
uint16_t fake_virtio_avail_count(const struct device *dev, uint16_t q);
/* Device side: mark a chain used with len bytes written, and raise the interrupt (ISR context). */
void fake_virtio_push_used(const struct device *dev, uint16_t q, uint16_t head, uint32_t len);
void fake_virtio_interrupt(const struct device *dev);

#endif
