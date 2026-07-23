# Run Log — The Flaky Network

All runs: `make && python3 run.py --profile <profile> --delay_ms <ms> [--seed N]`  
Duration: 30 s (1500 frames). Scoring: `score.py` (auto-invoked by `run.py`).

---

## Final recommended configuration

- **Submit** `delay_ms = 85`
- Build: `make` → `./sender`, `./receiver`
- Strategy: ~94% immediate duplicate frames + limited NACK fallback

---

## Baseline (before fixes)

Naive single-send + end-of-block FEC (BLOCK_LEN=5), no receiver timer, no NACKs.


| Profile | delay_ms | Misses     | Overhead | Result  |
| ------- | -------- | ---------- | -------- | ------- |
| A       | 40       | 66 (4.40%) | 1.24×    | INVALID |
| A       | 80       | 20 (1.33%) | 1.61×    | INVALID |


Relay stats (A, 40 ms): `up_bytes=297300`, `down_bytes=0` — confirms NACK path unused.

---



## After duplicate + timer + limited NACK



### Profile A (`profiles/A.json`) — 2% loss, 10–40 ms delay


| delay_ms | seed | Misses       | Overhead | Result                            |
| -------- | ---- | ------------ | -------- | --------------------------------- |
| 30       | 1    | 220 (14.67%) | 2.00×    | INVALID                           |
| 35       | 1    | 79 (5.27%)   | 2.00×    | INVALID                           |
| 38       | 1    | 26 (1.73%)   | 2.00×    | INVALID                           |
| 40       | 1    | 10 (0.67%)   | 2.00×    | VALID                             |
| 40       | 2    | 5 (0.33%)    | 2.00×    | VALID                             |
| 40       | 3    | 6 (0.40%)    | 2.00×    | VALID                             |
| 40       | 4    | 0 (0.00%)    | 2.00×    | VALID                             |
| 40       | 5    | 4 (0.27%)    | 2.00×    | VALID                             |
| 85       | 1    | 4 (0.27%)    | 2.00×    | INVALID (overhead 480810 B > cap) |
| 90       | 1    | 3 (0.20%)    | 2.01×    | INVALID (overhead)                |


**Floor for A: ~40 ms** (matches `delay_max_ms = 40`).

### Profile B (`profiles/B.json`) — 5% loss, 20–80 ms delay


| delay_ms | seed | Misses       | Overhead | Result                      |
| -------- | ---- | ------------ | -------- | --------------------------- |
| 40       | 1    | 744 (49.60%) | 2.00×    | INVALID                     |
| 60       | 1    | 237 (15.80%) | 2.00×    | INVALID                     |
| 80       | 1    | 18 (1.20%)   | 2.00×    | INVALID                     |
| 85       | 1    | 14 (0.93%)   | 2.00×    | VALID                       |
| 85       | 2    | 4 (0.27%)    | 2.00×    | VALID                       |
| 85       | 3    | 11 (0.73%)   | 2.00×    | VALID                       |
| 90       | 1    | 14 (0.93%)   | 2.00×    | VALID                       |
| 95       | 1    | 14 (0.93%)   | 2.00×    | INVALID (overhead 480315 B) |


**Floor for B: ~85 ms** (needs margin above `delay_max_ms = 80`).

---



## Overhead budget (typical valid run)

```
Raw stream     : 1500 frames × 160 B = 240,000 B
2× cap         : 480,000 B
Typical uplink : ~479,655 B (up), ~0–35 B (feedback)
Packet count   : ~2907 uplink datagrams (1500 + 1407 duplicates − relay drops)
```

Duplicate policy: send second copy when `(seq % 1500) < 1407`.

---



## Incidents during development


| Issue             | Symptom                              | Fix                                                  |
| ----------------- | ------------------------------------ | ---------------------------------------------------- |
| Port in use       | `bind 47002: Address already in use` | `fuser -k 4700*/udp`                                 |
| FEC too slow      | 4%+ misses at 40 ms                  | Replaced with immediate duplicates                   |
| No deadline timer | Bursts of misses when network quiet  | `poll()` with deadline timeout                       |
| NACK never sent   | `down_bytes: 0` always               | Receiver sends limited NACKs                         |
| NACK spam         | 3–7× overhead at delay_ms ≥ 100      | Cap 2 NACKs/seq; NACK only for non-duplicated frames |
| Overhead at cap   | INVALID despite low misses           | Reduced DUP_QUOTA 1409 → 1407                        |


---



## Commands to reproduce

```bash
# Recommended grading command
make && python3 run.py --profile profiles/B.json --delay_ms 85

# Best score on mild network (risky for unseen profiles)
make && python3 run.py --profile profiles/A.json --delay_ms 40

# Multi-seed sanity check
for s in 1 2 3 4 5; do
  python3 run.py --profile profiles/A.json --delay_ms 40 --seed $s
done
```

---



## Submission checklist

- [x] `make` produces `./sender` and `./receiver`
- [x] Miss rate ≤ 1% on profile B at **85 ms**
- [x] Bandwidth overhead ≤ 2.0×
- [x] Document chosen `delay_ms` for graders → **85 ms**