# Contributing

- **Supported Zephyr:** v4.4.x (the release gate, `ci.yml`, pinned in `west.yml`) and 4.5 / `main`
  (`zephyr-main-weekly.yml`, weekly, report-only). One source builds on both; the few places
  that differ are gated on `ZEPHYR_VERSION_CODE >= ZEPHYR_VERSION(4, 5, 0)`.
- **Adding or removing a test:** update the expected counts in
  `.github/workflows/zephyr-main-weekly.yml` (`EXPECT_TEST_CONFIGS`, `EXPECT_TEST_CASES`,
  `EXPECT_SAMPLE_CONFIGS`). The weekly job fails when the numbers differ, on purpose: a job
  that silently ran fewer tests would look green. The count must be the same on both
  Zephyr lines.
- **Run the tests locally:** `twister -T zephyr-virtio-can/tests -p qemu_cortex_a53`.
