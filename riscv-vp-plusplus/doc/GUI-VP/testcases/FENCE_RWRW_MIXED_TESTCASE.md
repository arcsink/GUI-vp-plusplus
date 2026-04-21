# `fence rw,rw` mixed access testcase

## 1. Goal

This testcase note focuses on Linux-side mixed access patterns around
`fence rw,rw`.

It covers three software-visible usage patterns:

1. `(写,写)` / write-write publish
2. `(读,读)` / read-read acquire
3. `(读,写)` / read-write handoff

The goal is not to prove the full memory model. The goal is to confirm that
the current GUI-VP path preserves the expected ordering for these three
common `fence rw,rw` software patterns.

## 2. Binary and run command

Source:
`doc/GUI-VP/testcases/src/guivp_fence_linux_suite.c`

Guest command:

```sh
/data/guivp_fence_linux_suite 2000
```

Logs and result notes should be stored under:

`doc/GUI-VP/testcases/results/`

## 3. Scenarios

### 3.1 `(写,写)` / write-write publish

Producer:

```text
payload = expected;
fence rw,rw;
flag = 1;
```

Consumer:

```text
while (flag == 0) ;
observed = payload;
```

Pass criteria:

- after consumer sees `flag == 1`, it must not observe stale `payload`

### 3.2 `(读,读)` / read-read acquire

Producer:

```text
payload = expected;
fence rw,rw;
flag = 1;
```

Consumer:

```text
while (flag == 0) ;
fence rw,rw;
observed = payload;
```

Pass criteria:

- after the read-side fence, consumer must not observe stale `payload`

### 3.3 `(读,写)` / read-write handoff

Producer:

```text
payload = expected;
fence rw,rw;
flag = 1;
```

Consumer:

```text
while (flag == 0) ;
fence rw,rw;
ack = 1;
```

Observer:

```text
while (ack == 0) ;
observed = payload;
```

Pass criteria:

- once observer sees `ack == 1`, it must observe the published `payload`
- stale `payload` after `ack == 1` counts as failure

## 4. Notes

These are regression-oriented Linux runtime checks for the current VP
implementation.

If future work needs stronger evidence, keep these as regression tests and
add more aggressive bare-metal or kernel-space litmus coverage.
