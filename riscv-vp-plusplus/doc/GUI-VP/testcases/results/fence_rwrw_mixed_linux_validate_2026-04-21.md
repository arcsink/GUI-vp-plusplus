# `fence rw,rw` mixed Linux validation

Date: 2026-04-21

## Goal

Validate three Linux-side mixed access patterns for `fence rw,rw`:

1. `(写,写)` / write-write publish
2. `(读,读)` / read-read acquire
3. `(读,写)` / read-write handoff

## Testcase note

See:
`doc/GUI-VP/testcases/FENCE_RWRW_MIXED_TESTCASE.md`

## Binary and command

Source:
`doc/GUI-VP/testcases/src/guivp_fence_linux_suite.c`

Guest command:

```sh
/data/guivp_fence_linux_suite 2000
```

## Runtime result

Observed output:

```text
[guivp-fence-suite] rwrw-mp label=write-angle-publish iterations=2000 producer_fence=1 consumer_fence=0 mismatches=0 expected=17
[guivp-fence-suite] rwrw-mp label=read-angle-acquire iterations=2000 producer_fence=1 consumer_fence=1 mismatches=0 expected=34
[guivp-fence-suite] rw-handoff label=read-write-handoff iterations=2000 mismatches=0 expected=51
[guivp-fence-suite] summary sb_no_fence_both_zero=0 sb_fence_rwrw_both_zero=0 sb_fence_tso_both_zero=0 failures=0
[guivp-fence-suite] RESULT=PASS note=sb-no-fence-did-not-hit-both-zero-in-this-run
```

## Conclusion

All three requested mixed scenarios passed in this run:

1. `(写,写)` / write-write publish: PASS (`mismatches=0/2000`)
2. `(读,读)` / read-read acquire: PASS (`mismatches=0/2000`)
3. `(读,写)` / read-write handoff: PASS (`mismatches=0/2000`)

This confirms that the current GUI-VP Linux-visible path keeps the expected
ordering for these three `fence rw,rw` software usage patterns.

## Notes

The existing fence regressions remained green in the same run:

- `sb mode=fence-rw-rw iterations=2000 both_zero=0`
- `fencei-remote label=remote-fence-i-rw-rw-publish ... result=3 expected=3`

As before, `sb mode=no-fence both_zero=0` in this run should not be interpreted
as a proof that the unfenced outcome is impossible.
