# FENCE.I Linux + VP Summary Validation (2026-04-21)

## Scope

- Continue the existing `FENCE.I` Linux-side validation flow.
- Observe VP-side `fence.i` activity without using verbose per-event trace.
- Use `RVVP_FENCEI_SUMMARY=1` and `Ctrl-A s` stats dump instead of `RVVP_FENCEI_TRACE=1`.

## Commands

Build the guest testcase:

```bash
buildroot_rv64/output/host/bin/riscv64-buildroot-linux-gnu-gcc \
  -O2 -pthread -Wall -Wextra \
  -o /tmp/guivp_fence_test/guivp_fence_linux_suite \
  riscv-vp-plusplus/doc/GUI-VP/testcases/src/guivp_fence_linux_suite.c
```

Run the VP directly to avoid unrelated full `vps` rebuild failures:

```bash
env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY \
  -u all_proxy -u ALL_PROXY \
  RVVP_FENCEI_SUMMARY=1 \
  riscv-vp-plusplus/vp/build/bin/linux64-mc-vp \
  --use-data-dmi --tlm-global-quantum=1000000 --use-dbbcache --use-lscache --tun-device tun10 \
  --dtb-file=dt/linux-vp_rv64_mc.dtb \
  --kernel-file buildroot_rv64/output/images/Image \
  --mram-root-image runtime_mram/mram_rv64_root.img \
  --mram-data-image runtime_mram/mram_rv64_data.img \
  --memory-size 2147483648 \
  buildroot_rv64/output/images/fw_jump.elf
```

Guest command:

```bash
/data/guivp_fence_linux_suite 2000
```

## Linux Result

```text
[guivp-fence-suite] cpu_count=4 sb_iterations=2000
[guivp-fence-suite] sb mode=no-fence iterations=2000 both_zero=0 elapsed_sec=18.633805
[guivp-fence-suite] sb mode=fence-rw-rw iterations=2000 both_zero=0 elapsed_sec=17.500395
[guivp-fence-suite] sb mode=fence-tso iterations=2000 both_zero=0 elapsed_sec=18.999993
[guivp-fence-suite] fencei-local warm=1 no_fence_ret=1 with_fence_i_ret=2
[guivp-fence-suite] fencei-remote label=no-remote-fence-i warmup=1 consumer_fence_i=0 result=1 expected=1
[guivp-fence-suite] fencei-remote label=remote-fence-i-rw-rw-publish warmup=1 consumer_fence_i=1 result=3 expected=3
[guivp-fence-suite] fencei-remote label=remote-fence-i-tso-publish warmup=1 consumer_fence_i=1 result=4 expected=4
[guivp-fence-suite] summary sb_no_fence_both_zero=0 sb_fence_rwrw_both_zero=0 sb_fence_tso_both_zero=0 failures=0
[guivp-fence-suite] RESULT=PASS note=sb-no-fence-did-not-hit-both-zero-in-this-run
```

## VP Summary Result

Collected through console stats (`Ctrl-A s`) with `RVVP_FENCEI_SUMMARY=1`:

```text
[RVVP fence.i summary] hart=0 events=1 fast_path_before=0 samples=1 coherence_now=1
[RVVP fence.i summary] hart=1 events=699 fast_path_before=693 samples=8 coherence_now=9670
[RVVP fence.i summary] hart=2 events=438 fast_path_before=432 samples=8 coherence_now=13344
[RVVP fence.i summary] hart=3 events=629 fast_path_before=620 samples=8 coherence_now=9110
[RVVP fence.i summary] hart=4 events=369 fast_path_before=362 samples=8 coherence_now=10261
```

Representative early samples:

```text
[RVVP fence.i summary] hart=0 sample=0 pc=0x8000063c coherence_before=0 coherence_after=1 fast_path_before=0
[RVVP fence.i summary] hart=4 sample=1 pc=0x8000063c coherence_before=1 coherence_after=2 fast_path_before=1
[RVVP fence.i summary] hart=1 sample=4 pc=0x8000ad48 coherence_before=8 coherence_after=9 fast_path_before=1
[RVVP fence.i summary] hart=2 sample=3 pc=0xffffffff8000f608 coherence_before=7482 coherence_after=7483 fast_path_before=1
```

## Notes

- An initial rerun against the previous guest binary hit `Illegal instruction` before `fencei-local` completed.
- The testcase was updated so the seeded warm-up code is synchronized explicitly before first execution:
  - same hart warm-up: `fence.i`
  - cross-hart seeded code publication: `fence rw,rw`
- The guest data image was checked before rerun; `/data/guivp_fence_linux_suite` matched the rebuilt binary hash
  `3c729ad133edc6a0d997b1f23ccab9a12480a904d3415ef6de3f62bc443bbcdc`.
- After rebuilding the guest testcase and updating the data image, the Linux suite passed end to end.
- The VP-side summary confirms that `fence.i` events are happening on multiple harts and that many of them are exiting the DBBCache fast path, without enabling noisy trace logs.
