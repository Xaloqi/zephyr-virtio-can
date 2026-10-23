# Changelog

## 0.1.0 (2026-10-23)

First tagged release, experimental. VIRTIO 1.4 CAN device driver for Zephyr v4.4.1:
classic CAN (standard and extended IDs), TX with completion callbacks, 16 RX buffers with
software filters, start/stop over the control queue, bus-off from the status field.
Unit-tested on `qemu_cortex_a53` with a fake virtio transport (classic, CAN FD and
RTR-refused variants). Run end-to-end under Xen 4.22 against rust-vmm
`vhost-device-can` in Zephyr-Reference-ECU (`xenvm/run.sh can`). CAN FD and RTR are not
tested against a real device.
