# t23_c3_shared

Shared interface layer for the T23N and ESP32-C3 rebuild projects.

## Why this folder matters

T23 and C3 must be built independently, but they still need one common source of
truth for:

- pin definitions
- SPI test expectations
- future packet definitions

That source of truth lives here.

## Contents

- `include/t23_c3_protocol.h`
  constants and reserved packet definitions
- `docs/pinmap.md`
  hardware mapping for the new architecture
- `docs/protocol.md`
  staged protocol evolution plan
- `docs/test_vectors.md`
  bring-up commands and expected outputs
- `docs/system_initialization_flow.md`
  full initialization sequence from power-on to SPI bring-up
- `docs/system_initialization_flow_zh.md`
  中文版初始化流程说明
- `docs/project_handover.md`
  newcomer-facing overview of the whole rebuild effort
