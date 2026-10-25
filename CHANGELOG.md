# Changelog

## 0.2.0 (2026-10-25)

- Supports Zephyr 4.5's multiple state-change callbacks (`can_add_state_change_callback()`),
  alongside Zephyr 4.4.x, from one source. Closes #4. On 4.5 bus-off is reported with
  `can_fire_state_change_callbacks()`, and the driver no longer implements the removed
  `.set_state_change_callback` hook. There is no behaviour change on 4.4.x (same object code).
  The optional `.state_change_callbacks_enabled` hook is not needed: the device delivers no
  state-change interrupt, so there is nothing to enable or disable.
- Tests use the callback list on 4.5 (two callbacks, both must fire) and the old call on 4.4.
- CI: the weekly, report-only job builds and tests against Zephyr `main` (SDK version taken
  from that tree, checksums from the SDK release) and keeps one tracking issue while it fails.
  The v4.4.1 job is unchanged and remains the release gate. CONTRIBUTING.md says when to
  update the job's expected test counts.

## 0.1.0 (2026-10-23)

First tagged release, experimental. VIRTIO 1.4 CAN device driver for Zephyr v4.4.1:
classic CAN (standard and extended IDs), TX with completion callbacks, 16 RX buffers with
software filters, start/stop over the control queue, bus-off from the status field.
Unit-tested on `qemu_cortex_a53` with a fake virtio transport (classic, CAN FD and
RTR-refused variants). Run end-to-end under Xen 4.22 against rust-vmm
`vhost-device-can` in Zephyr-Reference-ECU (`xenvm/run.sh can`). CAN FD and RTR are not
tested against a real device.
