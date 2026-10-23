# zephyr-virtio-can

A Zephyr driver for the **VIRTIO CAN device** (device ID 36), as an out-of-tree Zephyr
module. A Zephyr guest gets a CAN controller whose bus is owned by the device side,
typically a driver domain under a hypervisor. Applications use the standard Zephyr CAN
API (`can_send()`, `can_add_rx_filter*()`, ISO-TP), so they run unchanged.

**Status: experimental, 0.1.0.** It has been unit-tested, and run end-to-end in one setup
(below). It has not run on hardware, and nobody outside Xaloqi has reviewed it yet. Issues
and reports are welcome.

- **Specification:** OASIS VIRTIO 1.4, section "CAN Device"
  (`oasis-tcs/virtio-spec`, branch `virtio-1.4`, `device-types/can/description.tex`). The
  driver was written from that text.
- **Zephyr:** v4.4.1. It is built on Zephyr's virtio framework (`virtio_mmio` /
  `virtio_pci`, `virtqueue`).
- **Licence:** Apache-2.0.

## Use

Add the module to your workspace (`west.yml`), or pass it on the command line:

```
west build -b xenvm app -- -DZEPHYR_EXTRA_MODULES=/path/to/zephyr-virtio-can
```

Describe the device as a child of its virtio transport:

```dts
virtio_mmio@2000000 {
	compatible = "virtio,mmio";
	reg = <0x0 0x02000000 0x0 0x200>;
	interrupts = <GIC_SPI 1 IRQ_TYPE_LEVEL IRQ_DEFAULT_PRIORITY>;
	interrupt-parent = <&gic>;

	can0: virtio-can {
		compatible = "virtio,device24";   /* 36 = 0x24, the virtio,device<hex id> convention */
	};
};
```

It needs `CONFIG_CAN=y`, `CONFIG_VIRTIO=y` and a heap
(`CONFIG_HEAP_MEM_POOL_SIZE`), because Zephyr's virtio transports allocate virtqueues with
`k_malloc`. Footprint on arm64, per instance: about 6 KB of code and 2.3 KB of RAM.

| Kconfig | Default | |
|---|---|---|
| `CONFIG_CAN_VIRTIO_TX_SLOTS` | 4 | Frames in flight |
| `CONFIG_CAN_VIRTIO_RX_BUFFERS` | 16 | Buffers kept on the receive queue |
| `CONFIG_CAN_VIRTIO_MAX_FILTERS` | 16 | Software RX filters |
| `CONFIG_CAN_VIRTIO_CTRL_TIMEOUT_MS` | 1000 | Start/stop request timeout |

## Behaviour

| | |
|---|---|
| **Frames** | Classic CAN, standard and extended IDs. RTR if the device offers it. CAN FD with `CONFIG_CAN_FD_MODE=y` and a device that offers it. BRS and ESI cannot be expressed in VIRTIO 1.4: BRS is not forwarded, and frames with ESI are refused |
| **TX** | Completion callbacks give `0`, `-EIO` (the device refused), `-ENETDOWN` (stopped) or `-ENETUNREACH` (bus-off). `LATE_TX_ACK` is negotiated when offered |
| **RX** | Software filtering, standard and extended. Malformed buffers are dropped and re-queued |
| **Start/stop** | Over the control queue. A request that times out is remembered, and its buffers are not reused until the device answers |
| **Bit timing** | VIRTIO CAN has no guest-side timing: the controller belongs to the device side. `can_set_bitrate()` / `set_timing()` are accepted and ignored (`-EBUSY` while started), so applications that set a bitrate keep working |
| **State** | `ERROR_ACTIVE`, `STOPPED`, or `BUS_OFF` from the device's `status` field. The specification has no error counters, so they read 0 |
| **Modes** | `CAN_MODE_NORMAL`, plus `CAN_MODE_FD`. Loopback, listen-only and the other modes return `-ENOTSUP` |

**Known limits**
- Bus-off is detected when a transmission fails and in `can_get_state()`, not by
  interrupt. Zephyr 4.4.1's virtio transports do not deliver configuration-change
  interrupts.
- No grant-based DMA. Zephyr's virtqueue gives the device guest-physical addresses, so
  under Xen the backend needs foreign-mapping rights (`grant_usage=0`).
- RX buffers always have room for a 64-byte sdu, even for classic CAN (about 0.9 KB of
  RAM). The specification allows 8, but rust-vmm's `vhost-device-can` writes 64 and
  stops servicing its queues when a buffer is shorter.

## Tests

| | Where | What |
|---|---|---|
| Unit | `tests/drivers/can/virtio` on `qemu_cortex_a53` | A fake virtio transport plays the device. Completions go through Zephyr's own `virtio_isr()` in ISR context. Covered: negotiation, RX population before DRIVER_OK, TX encoding (std/ext/0-byte/RTR/FD), TX errors and slot exhaustion, stop with pending TX, RX filters and malformed buffers, control-queue rejection and timeout, bus-off. Three variants: classic, CAN FD, RTR refused |
| Integration | `samples/xenvm_selftest`, built for `xenvm` | Against a real device: [Zephyr-Reference-ECU](https://github.com/Xaloqi/Zephyr-Reference-ECU), branch `spike/virtio-can-frontend`, `xenvm/can-spike/`. That is Xen 4.22, with rust-vmm `vhost-device-can` behind `xen-vhost-frontend` in a driver domain. The same setup runs EDS answering UDS over ISO-TP through this driver |

```
west init -l zephyr-virtio-can && west update      # Zephyr v4.4.1, see west.yml
zephyr/scripts/twister -T zephyr-virtio-can/tests -p qemu_cortex_a53
```

The unit tests were checked against nine deliberately broken versions of the driver.
Eight make a test fail. The ninth, one extra TX permit, behaves identically, because
the slot search refuses anyway.

**Untested against a real device:** CAN FD and RTR (unit-tested only), bus-off, and
virtio-pci.
