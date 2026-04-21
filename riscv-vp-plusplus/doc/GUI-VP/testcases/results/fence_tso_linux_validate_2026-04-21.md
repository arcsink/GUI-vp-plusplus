# FENCE.TSO Linux Validation (2026-04-21)

## Scope

- Validate `fence.tso` once from the Linux guest side.
- Reuse the existing multicore Linux fence suite.
- Focus only on `fence.tso`-relevant observations instead of the full fence report.

## Run Path

Host-side VP launch:

```bash
env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY \
  -u all_proxy -u ALL_PROXY \
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

## Relevant Output

```text
[guivp-fence-suite] sb mode=fence-tso iterations=2000 both_zero=0 elapsed_sec=17.191232
[guivp-fence-suite] fencei-remote label=remote-fence-i-tso-publish warmup=1 consumer_fence_i=1 result=4 expected=4
[guivp-fence-suite] summary sb_no_fence_both_zero=0 sb_fence_rwrw_both_zero=0 sb_fence_tso_both_zero=0 failures=0
[guivp-fence-suite] RESULT=PASS note=sb-no-fence-did-not-hit-both-zero-in-this-run
```

## Conclusion

- `fence.tso` SB mode did not observe `both_zero`; this matches the testcase pass criterion for the Linux-side ordering check.
- In the remote publish scenario, producer-side `fence.tso` plus consumer-side local `fence.i` produced `result=4 expected=4`; this shows the current `fence.tso` path participates correctly in the tested Linux software synchronization flow.
- This run supports the existing scoped conclusion: the current implementation path is observable and not obviously broken in Linux guest validation.
- This run does not prove a stronger claim such as full global TSO semantics.
