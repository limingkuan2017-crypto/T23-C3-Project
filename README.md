# T23-C3-Project

Unified repository for the rebuilt T23N + ESP32-C3 system.

## Repository Layout

```text
T23-C3-Project/
├─ t23_rebuild/
├─ c3_rebuild/
├─ t23_c3_shared/
├─ third_party/
├─ docs/
└─ scripts/
```

## What Each Folder Does

- `t23_rebuild/`
  T23-side Linux userspace bring-up, camera diagnostics, SPI master diagnostics
- `c3_rebuild/`
  ESP32-C3 firmware used for SPI slave bring-up and later protocol work
- `t23_c3_shared/`
  shared protocol headers, pin map, handover docs and initialization flow docs
- `third_party/`
  local-only location for external dependencies not stored in Git
- `docs/`
  repository-level notes and onboarding documents
- `scripts/`
  repository-level helper scripts

## External Dependencies

To keep the repository portable and safe to share, external dependencies are
not committed here. Place them under `third_party/`:

- `third_party/ingenic_t23_sdk`
  extracted Ingenic T23 SDK root
- `third_party/vendor_reference`
  vendor reference project root

The T23 build scripts already look for dependencies in those locations.

## Quick Start

### T23

```sh
cd <repo>/t23_rebuild
./scripts/build_camera.sh
./scripts/package_flash_image.sh
```

### ESP32-C3

```sh
cd <repo>/c3_rebuild
./scripts/idf_build.sh
```

## First Documents To Read

- `t23_rebuild/docs/t23_runtime_flow.md`
- `t23_rebuild/docs/t23_function_guide_zh.md`
- `t23_rebuild/docs/t23_learning_path_zh.md`
- `t23_c3_shared/docs/system_initialization_flow_zh.md`
- `t23_c3_shared/docs/project_handover.md`
