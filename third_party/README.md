# third_party

This directory is reserved for local external dependencies that are required to
build the project but are not committed to Git.

Expected layout:

```text
third_party/
├─ ingenic_t23_sdk/
└─ vendor_reference/
```

## Why These Are Not Committed

- the Ingenic SDK is large
- the vendor reference project may contain closed or redistributable-limited
  content
- separating first-party code from external dependencies makes the repository
  easier to move between machines

## Required Entries

### `ingenic_t23_sdk`

Place the extracted Ingenic T23 SDK root here. The T23 build expects paths like:

```text
third_party/ingenic_t23_sdk/sdk/include
third_party/ingenic_t23_sdk/sdk/lib/uclibc
```

### `vendor_reference`

Place the vendor reference project root here. The T23 packaging flow expects:

```text
third_party/vendor_reference/build/images
third_party/vendor_reference/build/toolchain
```
