"""Python reference helpers for the unified T23/C3 rectification pipeline.

This module keeps the crop-selection logic numerically aligned with the
current T23 firmware implementation:

- build a direct valid mask from the rectified canvas
- convert invalid pixels to an integral image
- search the largest centered 16:9 crop while enforcing TM-based safe_top
- prefer fully valid crops before falling back to less-valid candidates

It is intended to be the Git-tracked Python reference for the desktop tuning
tool that lives outside this repository.
"""

from __future__ import annotations

import math
from typing import Tuple

import cv2
import numpy as np


def clamp(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))


def build_invalid_integral_image(valid_mask: np.ndarray) -> np.ndarray:
    """Build an integral image whose cells store invalid-pixel counts."""
    invalid = (valid_mask == 0).astype(np.uint32)
    h, w = invalid.shape
    integral = np.zeros((h + 1, w + 1), dtype=np.uint32)
    integral[1:, 1:] = invalid.cumsum(axis=0, dtype=np.uint32).cumsum(axis=1, dtype=np.uint32)
    return integral


def count_invalid_pixels(invalid_integral: np.ndarray, x: int, y: int, w: int, h: int) -> int:
    """Return invalid-pixel count inside the inclusive-exclusive ROI."""
    x2 = x + w
    y2 = y + h
    return int(
        invalid_integral[y2, x2]
        - invalid_integral[y, x2]
        - invalid_integral[y2, x]
        + invalid_integral[y, x]
    )


def score_centered_crop(
    invalid_integral: np.ndarray,
    x: int,
    y: int,
    w: int,
    h: int,
    desired_center: np.ndarray,
) -> Tuple[float, float]:
    """Score a crop by area, valid ratio, and distance from desired center."""
    total = w * h
    if total <= 0:
        return -1e18, 0.0

    invalid = count_invalid_pixels(invalid_integral, x, y, w, h)
    ratio = float(total - invalid) / float(total)
    crop_cx = x + w * 0.5
    crop_cy = y + h * 0.5
    dist2 = (crop_cx - desired_center[0]) ** 2 + (crop_cy - desired_center[1]) ** 2
    score = total + ratio * 1e6 - dist2 * 4.0
    return score, ratio


def find_centered_16x9_crop(
    valid_mask: np.ndarray,
    desired_center: np.ndarray,
    min_top: int = 0,
    min_valid_ratio: float = 1.0,
) -> Tuple[int, int, int, int, float]:
    """Search the largest feasible 16:9 crop using the same policy as T23."""
    h, w = valid_mask.shape
    invalid_integral = build_invalid_integral_image(valid_mask)
    target_ratio = 16.0 / 9.0

    best = None
    best_score = -1e18
    found_good = False
    min_top = int(clamp(min_top, 0, h - 1))

    for ch in range(h - min_top, 179, -4):
        cw = int(round(ch * target_ratio))
        if cw > w:
            continue

        x0 = int(round(desired_center[0] - cw * 0.5))
        y0 = int(round(desired_center[1] - ch * 0.5))
        x0 = int(clamp(x0, 0, w - cw))
        y0 = int(clamp(y0, min_top, h - ch))

        search_dx = min(40, max(8, cw // 40))
        search_dy = min(30, max(6, ch // 40))
        if min_top > 0:
            # Match firmware: if TM headroom constrains crop top, keep scanning
            # the whole feasible vertical span so bottom content is preserved.
            search_dy = max(0, h - ch - min_top)
        step_x = max(2, search_dx // 5)
        step_y = 2 if min_top > 0 else max(2, search_dy // 5)

        local_best = None
        local_best_score = -1e18

        for dy in range(-search_dy, search_dy + 1, step_y):
            for dx in range(-search_dx, search_dx + 1, step_x):
                x = int(clamp(x0 + dx, 0, w - cw))
                y = int(clamp(y0 + dy, min_top, h - ch))
                score, ratio = score_centered_crop(invalid_integral, x, y, cw, ch, desired_center)
                if score > local_best_score:
                    local_best_score = score
                    local_best = (x, y, cw, ch, ratio)

        if local_best is None:
            continue

        x, y, cw, ch, ratio = local_best
        if ratio >= min_valid_ratio:
            if (not found_good) or (local_best_score > best_score):
                best_score = local_best_score
                best = local_best
                found_good = True
            break

        if (not found_good) and local_best_score > best_score:
            best_score = local_best_score
            best = local_best

    if best is None:
        return 0, 0, w, h, 0.0
    return best


def undistorted_pixels_to_raw_pixels(
    undistorted_pts: np.ndarray,
    knew: np.ndarray,
    k: np.ndarray,
    d: np.ndarray,
) -> np.ndarray:
    """Map undistorted pixel coordinates back to raw fisheye pixels."""
    pts = np.asarray(undistorted_pts, dtype=np.float64).reshape(-1, 2)
    fx, fy = float(knew[0, 0]), float(knew[1, 1])
    cx, cy = float(knew[0, 2]), float(knew[1, 2])

    norm = np.empty_like(pts, dtype=np.float64)
    norm[:, 0] = (pts[:, 0] - cx) / fx
    norm[:, 1] = (pts[:, 1] - cy) / fy

    distorted = cv2.fisheye.distortPoints(
        norm.reshape(-1, 1, 2),
        K=k,
        D=d,
    )
    return distorted.reshape(-1, 2).astype(np.float32)


def build_direct_rectify_map_from_raw(
    hmat: np.ndarray,
    out_size: Tuple[int, int],
    knew: np.ndarray,
    k: np.ndarray,
    d: np.ndarray,
    raw_shape: Tuple[int, int, int],
) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Build direct raw lookup maps and the associated validity mask."""
    out_w, out_h = out_size
    raw_h, raw_w = raw_shape[:2]

    xs, ys = np.meshgrid(
        np.arange(out_w, dtype=np.float32),
        np.arange(out_h, dtype=np.float32),
    )
    dst_pts = np.stack([xs, ys], axis=-1).reshape(-1, 1, 2)
    h_inv = np.linalg.inv(hmat)
    ref_pts = cv2.perspectiveTransform(dst_pts, h_inv).reshape(-1, 2)
    raw_pts = undistorted_pixels_to_raw_pixels(ref_pts, knew, k, d)

    map_x = raw_pts[:, 0].reshape(out_h, out_w).astype(np.float32)
    map_y = raw_pts[:, 1].reshape(out_h, out_w).astype(np.float32)
    valid = (
        (map_x >= 0) & (map_x < raw_w) &
        (map_y >= 0) & (map_y < raw_h)
    ).astype(np.uint8)
    return map_x, map_y, valid


def rectify_from_raw_with_tm_crop(
    raw_img: np.ndarray,
    hmat: np.ndarray,
    out_size: Tuple[int, int],
    knew: np.ndarray,
    k: np.ndarray,
    d: np.ndarray,
    tm_rect_y: float,
    top_margin: int = 10,
):
    """Rectify raw fisheye input and crop with the same TM-safe policy as T23."""
    map_x, map_y, valid = build_direct_rectify_map_from_raw(
        hmat, out_size, knew, k, d, raw_img.shape
    )

    rectified_full = cv2.remap(
        raw_img,
        map_x,
        map_y,
        interpolation=cv2.INTER_CUBIC,
        borderMode=cv2.BORDER_CONSTANT,
        borderValue=(0, 0, 0),
    )

    out_w, out_h = out_size
    desired_center = np.array([out_w * 0.5, out_h * 0.5], dtype=np.float32)
    safe_top = int(clamp(math.floor(tm_rect_y - top_margin), 0, rectified_full.shape[0] - 1))

    x, y, cw, ch, ratio = find_centered_16x9_crop(
        valid,
        desired_center=desired_center,
        min_top=safe_top,
        min_valid_ratio=1.0,
    )

    rectified_crop = rectified_full[y:y + ch, x:x + cw]
    valid_crop = valid[y:y + ch, x:x + cw]
    final_valid_ratio = float(np.mean(valid_crop > 0)) if valid_crop.size else 0.0
    return rectified_full, rectified_crop, valid_crop, (x, y, cw, ch, final_valid_ratio)
