# Embedding response validation

Validate nonempty finite numeric vectors and recover indexed OpenAI response order.
Legacy all-unindexed responses retain input order; mixed, duplicate, negative,
noninteger and out-of-range indices fail. Preserve the original numeric JSON nodes.
No official fixtures or model names change. This is part of AI_EMBED, not the whole task.

- [ ] Actual-parser boundary tests RED before changes.
- [ ] Add validation and checked indexed order.
- [ ] Run component regressions and actual production object compilation.
- [ ] Save before investigating the two user-reported PowerContext failures.

JSON/allocation doubles cover control flow, not SQL integration. The native server
build runs independently; its result must be checked before claiming runtime success.
