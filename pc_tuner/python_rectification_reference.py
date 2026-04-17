"""Python reference helpers for the unified T23/C3 rectification pipeline.

This module tracks the current T23 firmware crop policy:

- estimate rectified canvas size from the fitted screen quad itself
- keep the TL/TR weak-reference top-edge model unchanged
- project required screen content into rectified space
- grow the largest fully valid crop around that required content
- prefer content completeness and no black border over a fixed 16:9 aspect
"""

from __future__ import annotations

import math
from typing import Tuple

import cv2
import numpy as np


POINT_NAMES = ["TL", "TM", "TR", "RM", "BR", "BM", "BL", "LM"]
IDX = {name: i for i, name in enumerate(POINT_NAMES)}


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


def rectified_rect_is_valid(
    invalid_integral: np.ndarray,
    left: int,
    top: int,
    right: int,
    bottom: int,
) -> bool:
    if left > right or top > bottom:
        return False
    return count_invalid_pixels(
        invalid_integral,
        left,
        top,
        right - left + 1,
        bottom - top + 1,
    ) == 0


def crop_aspect_error(width: int, height: int, desired_aspect: float) -> float:
    if width <= 0 or height <= 0 or desired_aspect <= 0.0:
        return float("inf")
    aspect = float(width) / float(height)
    return abs(math.log(aspect / desired_aspect))


def consider_crop_candidate(
    invalid_integral: np.ndarray,
    mask_shape: Tuple[int, int],
    left: int,
    top: int,
    right: int,
    bottom: int,
    desired_aspect: float,
    current_area: int,
    best,
):
    mask_h, mask_w = mask_shape
    if left < 0 or top < 0 or right >= mask_w or bottom >= mask_h:
        return best
    if not rectified_rect_is_valid(invalid_integral, left, top, right, bottom):
        return best

    width = right - left + 1
    height = bottom - top + 1
    area = width * height
    if area < current_area:
        return best

    error = crop_aspect_error(width, height, desired_aspect)
    if best is None or area > best[4] or (area == best[4] and error < best[5]):
        return (left, top, right, bottom, area, error)
    return best


def expand_valid_crop_greedy(
    invalid_integral: np.ndarray,
    valid_mask: np.ndarray,
    desired_aspect: float,
    left: int,
    top: int,
    right: int,
    bottom: int,
) -> Tuple[int, int, int, int]:
    mask_shape = valid_mask.shape
    while True:
        current_area = (right - left + 1) * (bottom - top + 1)
        best = None
        for candidate in (
            (left - 1, top, right, bottom),
            (left, top - 1, right, bottom),
            (left, top, right + 1, bottom),
            (left, top, right, bottom + 1),
            (left - 1, top, right + 1, bottom),
            (left, top - 1, right, bottom + 1),
            (left - 1, top - 1, right, bottom),
            (left, top - 1, right + 1, bottom),
            (left - 1, top, right, bottom + 1),
            (left, top, right + 1, bottom + 1),
            (left - 1, top - 1, right + 1, bottom + 1),
        ):
            best = consider_crop_candidate(
                invalid_integral,
                mask_shape,
                candidate[0],
                candidate[1],
                candidate[2],
                candidate[3],
                desired_aspect,
                current_area,
                best,
            )
        if best is None:
            break
        left, top, right, bottom = best[:4]
    return left, top, right, bottom


def find_nearest_valid_seed(valid_mask: np.ndarray, center_x: int, center_y: int) -> Tuple[int, int]:
    h, w = valid_mask.shape
    center_x = int(clamp(center_x, 0, w - 1))
    center_y = int(clamp(center_y, 0, h - 1))
    if valid_mask[center_y, center_x]:
        return center_x, center_y

    limit = max(w, h)
    for radius in range(1, limit):
        x0 = int(clamp(center_x - radius, 0, w - 1))
        x1 = int(clamp(center_x + radius, 0, w - 1))
        y0 = int(clamp(center_y - radius, 0, h - 1))
        y1 = int(clamp(center_y + radius, 0, h - 1))
        for x in range(x0, x1 + 1):
            if valid_mask[y0, x]:
                return x, y0
            if valid_mask[y1, x]:
                return x, y1
        for y in range(y0 + 1, y1):
            if valid_mask[y, x0]:
                return x0, y
            if valid_mask[y, x1]:
                return x1, y
    raise RuntimeError("no valid seed found in rectified mask")


def rectified_point_usable(pt: np.ndarray) -> bool:
    return bool(np.isfinite(pt[0]) and np.isfinite(pt[1]) and abs(pt[0]) < 100000.0 and abs(pt[1]) < 100000.0)


def accumulate_projected_content_point(hmat: np.ndarray, src_pt: np.ndarray, bounds):
    if not rectified_point_usable(src_pt):
        return bounds
    src = np.asarray(src_pt, dtype=np.float32).reshape(1, 1, 2)
    rect_pt = cv2.perspectiveTransform(src, hmat).reshape(2)
    if not np.isfinite(rect_pt).all():
        return bounds

    min_x, min_y, max_x, max_y, have_point = bounds
    if not have_point:
        return float(rect_pt[0]), float(rect_pt[1]), float(rect_pt[0]), float(rect_pt[1]), True
    return (
        min(min_x, float(rect_pt[0])),
        min(min_y, float(rect_pt[1])),
        max(max_x, float(rect_pt[0])),
        max(max_y, float(rect_pt[1])),
        True,
    )


def compute_required_content_bounds(
    pts_und: np.ndarray,
    hmat: np.ndarray,
    out_size: Tuple[int, int],
    top_corner_user_blend: float = 0.18,
) -> Tuple[int, int, int, int]:
    bounds = (0.0, 0.0, 0.0, 0.0, False)
    for name in ["TM", "LM", "RM", "BL", "BM", "BR"]:
        bounds = accumulate_projected_content_point(hmat, pts_und[IDX[name]], bounds)

    if rectified_point_usable(pts_und[IDX["BL"]]) and rectified_point_usable(pts_und[IDX["LM"]]):
        tl_pred = 2.0 * pts_und[IDX["LM"]] - pts_und[IDX["BL"]]
        if rectified_point_usable(pts_und[IDX["TL"]]):
            tl_ref = tl_pred * (1.0 - top_corner_user_blend) + pts_und[IDX["TL"]] * top_corner_user_blend
        else:
            tl_ref = tl_pred
        bounds = accumulate_projected_content_point(hmat, tl_ref, bounds)

    if rectified_point_usable(pts_und[IDX["BR"]]) and rectified_point_usable(pts_und[IDX["RM"]]):
        tr_pred = 2.0 * pts_und[IDX["RM"]] - pts_und[IDX["BR"]]
        if rectified_point_usable(pts_und[IDX["TR"]]):
            tr_ref = tr_pred * (1.0 - top_corner_user_blend) + pts_und[IDX["TR"]] * top_corner_user_blend
        else:
            tr_ref = tr_pred
        bounds = accumulate_projected_content_point(hmat, tr_ref, bounds)

    min_x, min_y, max_x, max_y, have_point = bounds
    if not have_point:
        raise RuntimeError("failed to compute required content bounds")

    out_w, out_h = out_size
    left = int(clamp(math.floor(min_x), 0, out_w - 1))
    top = int(clamp(math.floor(min_y), 0, out_h - 1))
    right = int(clamp(math.ceil(max_x), left, out_w - 1))
    bottom = int(clamp(math.ceil(max_y), top, out_h - 1))
    return left, top, right, bottom


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
    pts_und: np.ndarray | None = None,
    top_corner_user_blend: float = 0.18,
):
    """Rectify raw fisheye input and crop using the current T23 content-first policy."""
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
    desired_aspect = float(out_w) / float(out_h) if out_h > 0 else 16.0 / 9.0
    invalid_integral = build_invalid_integral_image(valid)

    if pts_und is not None:
        left, top, right, bottom = compute_required_content_bounds(
            pts_und, hmat, out_size, top_corner_user_blend=top_corner_user_blend
        )
        if not rectified_rect_is_valid(invalid_integral, left, top, right, bottom):
            seed_x, seed_y = find_nearest_valid_seed(valid, int(round(out_w * 0.5)), int(round(out_h * 0.5)))
            left = right = seed_x
            top = bottom = seed_y
    else:
        seed_x, seed_y = find_nearest_valid_seed(valid, int(round(out_w * 0.5)), int(round(out_h * 0.5)))
        left = right = seed_x
        top = bottom = seed_y

    left, top, right, bottom = expand_valid_crop_greedy(
        invalid_integral,
        valid,
        desired_aspect,
        left,
        top,
        right,
        bottom,
    )

    x = left
    y = top
    cw = right - left + 1
    ch = bottom - top + 1
    rectified_crop = rectified_full[y:y + ch, x:x + cw]
    valid_crop = valid[y:y + ch, x:x + cw]
    final_valid_ratio = float(np.mean(valid_crop > 0)) if valid_crop.size else 0.0
    return rectified_full, rectified_crop, valid_crop, (x, y, cw, ch, final_valid_ratio)
