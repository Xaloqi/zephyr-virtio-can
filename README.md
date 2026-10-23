# zephyr-virtio-can (spike)

A Zephyr driver for the VIRTIO CAN device (device ID 36), packaged as an out-of-tree Zephyr module.

**State: spike, 2026-10-23. Not a release.** It was written and tested in one session
to answer one question: can a Zephyr guest under Xen (SoDeV's "DomZ") use a CAN bus
served by a driver domain through virtio-can, as in AGL SoDeV's architecture (A)?
The answer is yes, against the setup below. It has not been reviewed, fuzzed,
or run on hardware, and it has no unit tests of its own.

- **Licence:** Apache-2.0 (SPDX headers in every file). Where it is published is
  still undecided.
- **Specification:** OASIS VIRTIO 1.4, section "CAN Device"
  (`oasis-tcs/virtio-spec`, branch `virtio-1.4`, `device-types/can/description.tex`
  at `6c2340547951`). The driver was written from that text. No code was taken from
  the Linux driver (`drivers/net/can/virtio_can.c`, GPL). Linux 7.2.12 was only run
  as a reference guest, to test the backend.
- **Zephyr:** v4.4.1 (`1f6485eca254`), SoDeV's pin. It uses Zephyr's own virtio
  framework (`virtio_mmio`, `virtqueue`) and implements the standard CAN driver API,
  so `can_send()`, `can_add_rx_filter*()` and ISO-TP users run unchanged.

## Use

```
west build -b xenvm <app> -- -DZEPHYR_EXTRA_MODULES=/path/to/zephyr-virtio-can
```

```dts
virtio_mmio@2000000 {
	compatible = "virtio,mmio";
	reg = <0x0 0x02000000 0x0 0x200>;
	interrupts = <GIC_SPI 1 IRQ_TYPE_LEVEL IRQ_DEFAULT_PRIORITY>;
	interrupt-parent = <&gic>;

	can0: virtio-can {
		compatible = "virtio,device24";   /* 36 = 0x24, as virtio,device4 for rng */
	};
};
```

It needs `CONFIG_CAN=y`, `CONFIG_VIRTIO=y` and a heap
(`CONFIG_HEAP_MEM_POOL_SIZE`). Zephyr's virtio transport allocates the virtqueues
with `k_malloc`.

## What it implements

| Spec / API | Behaviour |
|---|---|
| Feature negotiation | `CAN_CLASSIC` (required), `RTR_FRAMES` and `LATE_TX_ACK` if offered, `CAN_FD` if offered and `CONFIG_CAN_FD_MODE=y` |
| Txq | `CONFIG_CAN_VIRTIO_TX_SLOTS` frames in flight (default 4). The callback runs on completion: `0`, `-EIO` (device said NOT_OK), `-ENETDOWN` (stopped), `-ENETUNREACH` (bus-off). A blocking `can_send()` works through Zephyr's wrapper |
| Rxq | `CONFIG_CAN_VIRTIO_RX_BUFFERS` buffers (default 16), queued before DRIVER_OK as the spec requires, re-queued from the ISR |
| Filters | Software matching (`can_frame_matches_filter`) against up to `CONFIG_CAN_VIRTIO_MAX_FILTERS` filters, standard and extended |
| Controlq | `can_start()` / `can_stop()` send `SET_CTRL_MODE_START/STOP` and wait up to `CONFIG_CAN_VIRTIO_CTRL_TIMEOUT_MS` for the result |
| Modes | `CAN_MODE_NORMAL`, plus `CAN_MODE_FD` when negotiated. Loopback, listen-only and the others return `-ENOTSUP` |
| Timing | The spec has no bit-timing configuration: the controller belongs to the device side. `set_timing()` / `can_set_bitrate()` are accepted and ignored (`-EBUSY` while started), so apps that set a bitrate at init keep working. `get_core_clock()` returns a nominal 80 MHz for Zephyr's timing calculator |
| State | `ERROR_ACTIVE` when started, `STOPPED` when stopped, `BUS_OFF` from the config `status` bit. Error counters are always 0 (not in the spec) |

## Not implemented / known gaps

- **Bus-off notification is polled, not event-driven.** Zephyr 4.4.1's virtio
  transport does not deliver configuration-change interrupts (it logs an error).
  The driver reads `status` when a transmission fails and in `get_state()`, and only
  then calls the state-change callback.
- **CAN FD: compiled but untested.** The FD path builds with `CONFIG_CAN_FD_MODE`,
  but no FD frame has crossed it. BRS and ESI cannot be expressed in VIRTIO 1.4,
  so they are dropped.
- **RTR: untested.**
- **Grant-based DMA (`grant_usage=1`) is not supported.** Zephyr's virtqueue hands
  the device guest-physical addresses. This driver was only run with
  `grant_usage=0`, the device side having foreign-mapping rights.
- **A control request that times out stays queued.** A later request can then race
  with it. Acceptable for a spike, not for a product.
- **No unit tests.** It was exercised by an integration test app (in the spike
  sandbox, `q2/app`) against the real backend. A product version needs a fake
  virtio transport and ztest coverage.
- **RX buffers are sized for a 64-byte sdu even without CAN FD** (about 0.9 KB
  more RAM). The spec allows 8, but rust-vmm's `vhost-device-can` writes the full
  64-byte struct and stops servicing its queues when a buffer is shorter.

## Tested against

Xen 4.22.0 (`d45d5687f1`) on QEMU 10.0.13 `virt` (arm64, TCG). Dom0 and DomD run
Linux 6.18.57. DomD is a dom0less domain with `capabilities=<1>`. Its backends are
rust-vmm `vhost-device-can` 0.1.0 (`vhost-device` `c0bfee92`, `--features xen`) on a
`vcan` interface, with Linaro's `xen-vhost-frontend` (`a1e333c5`) patched as
recorded in the campaign record. The guest uses virtio-mmio with
`grant_usage=0`.

The record is in `xaloqi-knowledge`,
`campaigns/2026-10-23-virtio-can-frontend-spike.md`. It covers the patches, the
bytes on the wire, and EDS answering UDS over ISO-TP through this driver.
