# TwoLeftHashMap architecture

This document explains the basic design of the experimental 2-left hash map.

## Big idea

The implementation is intentionally split into two layers:

```text
TwoLeftHashMap
    dynamic public wrapper
    owns growth, rebuild, and stash-pressure policy

FixedTwoLeftTable
    fixed-capacity storage image
    owns exact placement inside one concrete layout
    never resizes itself
```

The reason for this split is that **placement mechanics** and **growth policy** are different concerns. The fixed table answers: "can this key/value pair be placed in this concrete table image?" The wrapper answers: "when should this table image be replaced by a better one?"

## Physical layout

Inside `FixedTwoLeftTable`, all entry slots are stored in one vector of raw slots.
Logically, that vector is divided like this:

```text
slots_:

|---------------- table 1 ----------------|---------------- table 2 ----------------|------ stash ------|
| bucket 0 | bucket 1 | ... | bucket n-1 | bucket 0 | bucket 1 | ... | bucket n-1 | 0 | 1 | ... |
| s0 s1 s2 | s0 s1 s2 |     | s0 s1 s2   | s0 s1 s2 | s0 s1 s2 |     | s0 s1 s2   | entries     |
```

With `BucketSlots = 4`, one bucket looks like this:

```text
load = 0: [ raw  ][ raw  ][ raw  ][ raw  ]
load = 1: [ used ][ raw  ][ raw  ][ raw  ]
load = 2: [ used ][ used ][ raw  ][ raw  ]
load = 3: [ used ][ used ][ used ][ raw  ]
load = 4: [ used ][ used ][ used ][ used ]
```

The table stores only bucket loads, not tombstones. The occupied part of each bucket is always the prefix `[0, load)`.

## Insertion

For a key `k`, the table computes two candidate buckets:

```text
b1 = h1(k) & (bucket_count - 1)
b2 = h2(k) & (bucket_count - 1)
```

Because `bucket_count` is a power of two, masking replaces modulo division.
The table derives `h1` and `h2` by combining the user's base hash with two different seeds and then applying a 64-bit mixing function.

Insertion uses the 2-left rule:

```text
if load(table1[b1]) <= load(table2[b2]):
    try table1[b1]
else:
    try table2[b2]

if the selected bucket is full:
    use the stash

if the stash is full:
    report InsertStatus::full to the wrapper
```

The fixed table never grows itself.

## Lookup

Lookup checks exactly the places where a key may legally be stored:

```text
find(k):
    b1 = h1(k) & mask
    scan occupied slots of table1[b1]

    b2 = h2(k) & mask
    scan occupied slots of table2[b2]

    scan the stash
```

This is deliberately simple. A later version could add fingerprints/tags to reject most candidate slots before comparing full keys.

## Erase

Since buckets are compact, erasing from a main bucket moves the last occupied slot of that bucket into the removed position:

```text
before erase of B:
    [ A ][ B ][ C ][ D ]  load = 4

move D into B's old position:
    [ A ][ D ][ C ][ raw ]  load = 3
```

The order inside a bucket is not stable, but no tombstones are needed.

After erasing from a main bucket, the fixed table tries to promote stash entries back into their legal main buckets. This keeps the stash from remaining polluted after space becomes available.

## Rebuild and growth

The wrapper rebuilds the fixed table when:

- inserting one more entry would exceed `max_load_factor`,
- the stash becomes too occupied,
- the fixed table reports `InsertStatus::full`.

Rebuilding is transactional:

```text
old table remains alive
    |
    | copy/reinsert entries into fresh table
    v
fresh candidate table
    |
    | if successful
    v
move fresh table into wrapper
```

If a same-size rebuild with new seeds still creates too much stash pressure, the wrapper grows the bucket count and tries again.

## What is measured separately

For this table, useful metrics include:

```text
size
main_capacity
load_factor
stash_size
stash_capacity
insert time
successful lookup time
failed lookup time
bucket-load histogram
resize count
same-size rehash count
```

The stash is especially important: it is an overflow mechanism, not normal capacity. A large stash means the current size, seeds, or bucket count are no longer ideal.
