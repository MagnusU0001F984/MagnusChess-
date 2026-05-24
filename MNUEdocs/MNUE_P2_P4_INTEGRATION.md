# MNUE P2/P4 Integration Notes

This patch adds a first integration scaffold for the two-stage MNUE evaluator.

## Architecture

P2 is the fast filter network:

```text
MNUE-P2 = 1 × 32 × 16 × 1024 × 10240
```

P4 is the precision refine network:

```text
MNUE-P4 = 1 × 32 × 32 × 5120 × 20480
```

The feature decomposition is:

```text
input_size = input_bucket × relative_color × nonking_piece_type × relative_square
P2: 16 × 2 × 5 × 64 = 10240
P4: 32 × 2 × 5 × 64 = 20480
```

Kings are not input features. The input bucket is derived from the perspective-relative own king zone.

The output head has 32 buckets:

```text
output_bucket = phase_bucket2 × stm_king_zone16
```

## Search policy

P2 is the base evaluator. P4 is used only as a lazy refine evaluator in selected PVS nodes:

- PV node with depth >= `MAGNUS_MNUE_P4_PV_MIN_DEPTH`
- depth >= `MAGNUS_MNUE_P4_DEEP_MIN_DEPTH`
- in-check node with depth >= `MAGNUS_MNUE_P4_CHECK_MIN_DEPTH`
- static eval close to alpha/beta boundary

TT raw eval and correction-history base eval remain P2-based in this first patch. P4 is deliberately not stored as TT eval.

## File format

MNUE files use a required header:

```cpp
struct MnueHeader {
    uint32_t magic;          // "MNUE" little-endian
    uint32_t version;        // 1
    uint32_t arch;           // 2 for P2, 4 for P4
    uint32_t input_size;     // 10240 / 20480
    uint32_t hidden_size;    // 1024 / 5120
    uint32_t input_buckets;  // 16 / 32
    uint32_t output_buckets; // 32
    int32_t scale;           // usually 400
    int32_t qa;              // 255
    int32_t qb;              // 64
};
```

Weight order:

```text
header
w0: input_size × hidden_size × i16
b0: hidden_size × i16
w1: output_buckets × 2 × hidden_size × i16
b1: output_buckets × i16
```

## Important implementation status

This patch intentionally uses lazy rebuild for both P2 and P4 to make the first integration easy to verify.

The next performance patch should move P2 to a Position-resident incremental accumulator and keep P4 lazy.
