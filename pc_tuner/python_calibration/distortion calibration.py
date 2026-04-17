import json
import math
import os
from dataclasses import dataclass

import cv2
import numpy as np


ROOT_DIR = r"C:\Code\T23-C3-Project\Python"
IMAGES_DIR = os.path.join(ROOT_DIR, "Images")
CALIBRATION_FILE = os.path.join(ROOT_DIR, "fisheye_charuco_calibration.npz")
POINTS_FILE = os.path.join(ROOT_DIR, "distortion_points.json")

WINDOW_NAME = "Distortion Calibration"
PANEL_W = 640
PANEL_H = 320
PANEL_GAP = 24
HEADER_H = 48
FOOTER_H = 54

RECTIFIED_MAX_W = 1280
RECTIFIED_MAX_H = 720

POINT_NAMES = ["TL", "TM", "TR", "RM", "BR", "BM", "BL", "LM"]
IDX = {name: i for i, name in enumerate(POINT_NAMES)}

DEFAULT_POINTS_NORM = np.array(
    [
        [0.02, 0.42],  # TL
        [0.50, 0.23],  # TM
        [0.98, 0.42],  # TR
        [0.90, 0.54],  # RM
        [0.80, 0.65],  # BR
        [0.50, 0.64],  # BM
        [0.18, 0.64],  # BL
        [0.08, 0.54],  # LM
    ],
    dtype=np.float32,
)


@dataclass
class Line2D:
    a: float
    b: float
    c: float


def clamp(value, lo, hi):
    return max(lo, min(hi, value))


def point_distance(a, b):
    return float(np.linalg.norm(a - b))


def fit_line_tls(points):
    pts = np.asarray(points, dtype=np.float64)
    if len(pts) < 2:
        raise ValueError("need at least 2 points")
    mean = pts.mean(axis=0)
    centered = pts - mean
    cov = centered.T @ centered
    vals, vecs = np.linalg.eigh(cov)
    direction = vecs[:, np.argmax(vals)]
    direction = direction / np.linalg.norm(direction)
    a = -direction[1]
    b = direction[0]
    c = -(a * mean[0] + b * mean[1])
    norm = math.hypot(a, b)
    return Line2D(a / norm, b / norm, c / norm)


def intersect_lines(l1, l2):
    det = l1.a * l2.b - l2.a * l1.b
    if abs(det) < 1e-9:
        raise ValueError("parallel lines")
    x = (l1.b * l2.c - l2.b * l1.c) / det
    y = (l2.a * l1.c - l1.a * l2.c) / det
    return np.array([x, y], dtype=np.float32)


def compute_rectified_size_from_quad(quad):
    top_len = point_distance(quad[0], quad[1])
    right_len = point_distance(quad[1], quad[2])
    bottom_len = point_distance(quad[2], quad[3])
    left_len = point_distance(quad[3], quad[0])

    width_est = max(top_len, bottom_len)
    height_est = max(left_len, right_len)

    scale = 1.0

    if width_est < 1.0:
        width_est = 320.0
    if height_est < 1.0:
        height_est = 180.0

    if width_est < 320.0 or height_est < 180.0:
        min_scale_w = 320.0 / width_est
        min_scale_h = 180.0 / height_est
        scale = max(min_scale_w, min_scale_h)

    if width_est * scale > RECTIFIED_MAX_W or height_est * scale > RECTIFIED_MAX_H:
        max_scale_w = RECTIFIED_MAX_W / width_est
        max_scale_h = RECTIFIED_MAX_H / height_est
        scale = min(max_scale_w, max_scale_h)

    width = int(round(width_est * scale))
    height = int(round(height_est * scale))

    width = clamp(width, 320, RECTIFIED_MAX_W)
    height = clamp(height, 180, RECTIFIED_MAX_H)
    return int(width), int(height)


def load_calibration():
    data = np.load(CALIBRATION_FILE)
    k = data["K"].astype(np.float64)
    d = data["D"].astype(np.float64)
    image_size = tuple(int(v) for v in data["image_size"])
    return k, d, image_size


def build_knew(k, scale):
    knew = k.copy()
    knew[0, 0] *= scale
    knew[1, 1] *= scale
    return knew


def undistort_points(points, k, d, knew):
    pts = np.asarray(points, dtype=np.float64).reshape(-1, 1, 2)
    und = cv2.fisheye.undistortPoints(pts, k, d, P=knew)
    return und.reshape(-1, 2).astype(np.float32)


def undistort_image(img, k, d, knew):
    h, w = img.shape[:2]
    map1, map2 = cv2.fisheye.initUndistortRectifyMap(
        k, d, np.eye(3), knew, (w, h), cv2.CV_16SC2
    )
    return cv2.remap(
        img,
        map1,
        map2,
        interpolation=cv2.INTER_LINEAR,
        borderMode=cv2.BORDER_CONSTANT,
        borderValue=(0, 0, 0),
    )


def points_to_dict(points):
    return {
        POINT_NAMES[i]: [float(points[i][0]), float(points[i][1])]
        for i in range(len(POINT_NAMES))
    }


def dict_to_points(data, w, h):
    pts = []
    for i, name in enumerate(POINT_NAMES):
        if isinstance(data, dict) and name in data:
            pts.append(data[name])
        else:
            default_pt = DEFAULT_POINTS_NORM[i] * np.array([w, h], dtype=np.float32)
            pts.append(default_pt.tolist())
    return np.array(pts, dtype=np.float32)


def load_saved_points():
    if not os.path.exists(POINTS_FILE):
        return {}
    with open(POINTS_FILE, "r", encoding="utf-8") as f:
        return json.load(f)


def save_saved_points(data):
    with open(POINTS_FILE, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)


def constrain_points(points, horizontal_lock=True, symmetry_lock=False):
    pts = points.copy()
    if horizontal_lock:
        for a, b in [("TL", "TR"), ("LM", "RM"), ("BL", "BR")]:
            ia = IDX[a]
            ib = IDX[b]
            y = 0.5 * (pts[ia, 1] + pts[ib, 1])
            pts[ia, 1] = y
            pts[ib, 1] = y
    if symmetry_lock:
        center_x = 0.5 * (pts[IDX["TM"], 0] + pts[IDX["BM"], 0])
        for left_name, right_name in [("TL", "TR"), ("LM", "RM"), ("BL", "BR")]:
            il = IDX[left_name]
            ir = IDX[right_name]
            dist = 0.5 * ((center_x - pts[il, 0]) + (pts[ir, 0] - center_x))
            pts[il, 0] = center_x - dist
            pts[ir, 0] = center_x + dist
    return pts


def solve_rectification(points_raw, k, d, knew, top_corner_user_blend=0.18):
    pts_und = undistort_points(points_raw, k, d, knew)

    bottom_line = fit_line_tls([pts_und[IDX["BR"]], pts_und[IDX["BM"]], pts_und[IDX["BL"]]])
    left_line = fit_line_tls([pts_und[IDX["BL"]], pts_und[IDX["LM"]]])
    right_line = fit_line_tls([pts_und[IDX["BR"]], pts_und[IDX["RM"]]])

    tl_pred = 2.0 * pts_und[IDX["LM"]] - pts_und[IDX["BL"]]
    tr_pred = 2.0 * pts_und[IDX["RM"]] - pts_und[IDX["BR"]]

    tl_ref = tl_pred * (1.0 - top_corner_user_blend) + pts_und[IDX["TL"]] * top_corner_user_blend
    tr_ref = tr_pred * (1.0 - top_corner_user_blend) + pts_und[IDX["TR"]] * top_corner_user_blend
    top_line = fit_line_tls([tl_ref, tr_ref])

    quad = np.array(
        [
            intersect_lines(top_line, left_line),   # TL
            intersect_lines(top_line, right_line),  # TR
            intersect_lines(bottom_line, right_line),  # BR
            intersect_lines(bottom_line, left_line),   # BL
        ],
        dtype=np.float32,
    )

    rect_w, rect_h = compute_rectified_size_from_quad(quad)
    dst_quad = np.array(
        [[0, 0], [rect_w - 1, 0], [rect_w - 1, rect_h - 1], [0, rect_h - 1]],
        dtype=np.float32,
    )
    hmat = cv2.getPerspectiveTransform(quad, dst_quad)
    return pts_und, quad, hmat, (rect_w, rect_h)


def undistorted_pixels_to_raw_pixels(points_uv, knew, k, d):
    pts = np.asarray(points_uv, dtype=np.float64).reshape(-1, 2)

    fx = knew[0, 0]
    fy = knew[1, 1]
    cx = knew[0, 2]
    cy = knew[1, 2]

    x = (pts[:, 0] - cx) / fx
    y = (pts[:, 1] - cy) / fy
    und_norm = np.stack([x, y], axis=1).reshape(-1, 1, 2)

    raw_pts = cv2.fisheye.distortPoints(und_norm, k, d)
    return raw_pts.reshape(-1, 2).astype(np.float32)


def build_direct_rectify_map_from_raw(hmat, out_size, knew, k, d, raw_shape):
    out_w, out_h = out_size
    raw_h, raw_w = raw_shape[:2]

    xs, ys = np.meshgrid(
        np.arange(out_w, dtype=np.float32),
        np.arange(out_h, dtype=np.float32)
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


def build_invalid_integral_image(valid_mask):
    invalid = (valid_mask == 0).astype(np.uint32)
    h, w = invalid.shape
    integral = np.zeros((h + 1, w + 1), dtype=np.uint32)
    integral[1:, 1:] = invalid.cumsum(axis=0, dtype=np.uint32).cumsum(axis=1, dtype=np.uint32)
    return integral


def count_invalid_pixels(invalid_integral, x, y, w, h):
    x2 = x + w
    y2 = y + h
    return int(
        invalid_integral[y2, x2]
        - invalid_integral[y, x2]
        - invalid_integral[y2, x]
        + invalid_integral[y, x]
    )


def rectified_rect_is_valid(invalid_integral, left, top, right, bottom):
    if left > right or top > bottom:
        return False
    return count_invalid_pixels(
        invalid_integral,
        left,
        top,
        right - left + 1,
        bottom - top + 1,
    ) == 0


def crop_aspect_error(width, height, desired_aspect):
    if width <= 0 or height <= 0 or desired_aspect <= 0.0:
        return float("inf")
    aspect = float(width) / float(height)
    return abs(math.log(aspect / desired_aspect))


def consider_crop_candidate(invalid_integral, mask_shape, left, top, right, bottom, desired_aspect, current_area, best):
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


def expand_valid_crop_greedy(invalid_integral, valid_mask, desired_aspect, left, top, right, bottom):
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


def find_nearest_valid_seed(valid_mask, center_x, center_y):
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


def rectified_point_usable(pt):
    return bool(np.isfinite(pt[0]) and np.isfinite(pt[1]) and abs(pt[0]) < 100000.0 and abs(pt[1]) < 100000.0)


def accumulate_projected_content_point(hmat, src_pt, bounds):
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


def compute_required_content_bounds(pts_und, hmat, out_size, top_corner_user_blend=0.18):
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


def rectify_from_raw_with_tm_crop(
    raw_img,
    hmat,
    out_size,
    knew,
    k,
    d,
    tm_rect_y,
    top_margin=10,
    pts_und=None,
    top_corner_user_blend=0.18,
):
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


class CalibrationApp:
    def __init__(self):
        self.k, self.d, self.image_size = load_calibration()

        self.image_paths = sorted(
            os.path.join(IMAGES_DIR, name)
            for name in os.listdir(IMAGES_DIR)
            if name.lower().endswith((".jpg", ".jpeg", ".png"))
        )
        if not self.image_paths:
            raise RuntimeError("No images found in Images directory.")

        self.saved_points = load_saved_points()
        self.image_index = 0
        self.drag_index = -1

        self.knew_scale = 0.56
        self.top_corner_user_blend = 0.18
        self.horizontal_lock = True
        self.symmetry_lock = False

        self.load_image()

    def load_image(self):
        self.image = cv2.imread(self.image_paths[self.image_index])
        if self.image is None:
            raise RuntimeError(f"Failed to read image: {self.image_paths[self.image_index]}")
        self.image = cv2.resize(self.image, (PANEL_W, PANEL_H), interpolation=cv2.INTER_AREA)

        key = os.path.basename(self.image_paths[self.image_index])

        if key in self.saved_points:
            saved = self.saved_points[key]

            if isinstance(saved, dict):
                self.points = dict_to_points(saved, PANEL_W, PANEL_H)
            else:
                self.points = np.array(saved, dtype=np.float32)
        else:
            self.points = (DEFAULT_POINTS_NORM * np.array([PANEL_W, PANEL_H], dtype=np.float32)).astype(np.float32)

        self.points = constrain_points(self.points, self.horizontal_lock, self.symmetry_lock)

    def save_points(self):
        key = os.path.basename(self.image_paths[self.image_index])
        self.saved_points[key] = points_to_dict(self.points)
        save_saved_points(self.saved_points)

    def reset_points(self):
        self.points = (DEFAULT_POINTS_NORM * np.array([PANEL_W, PANEL_H], dtype=np.float32)).astype(np.float32)
        self.points = constrain_points(self.points, self.horizontal_lock, self.symmetry_lock)

    def mouse(self, event, x, y, flags, param):
        raw_x0 = 0
        raw_y0 = HEADER_H

        if not (raw_x0 <= x < raw_x0 + PANEL_W and raw_y0 <= y < raw_y0 + PANEL_H):
            if event == cv2.EVENT_LBUTTONUP:
                self.drag_index = -1
            return

        local_x = x - raw_x0
        local_y = y - raw_y0

        if event == cv2.EVENT_LBUTTONDOWN:
            dists = np.linalg.norm(self.points - np.array([local_x, local_y]), axis=1)
            idx = int(np.argmin(dists))
            if dists[idx] <= 18:
                self.drag_index = idx
        elif event == cv2.EVENT_MOUSEMOVE and self.drag_index >= 0:
            self.points[self.drag_index] = [clamp(local_x, 0, PANEL_W - 1), clamp(local_y, 0, PANEL_H - 1)]
            self.points = constrain_points(self.points, self.horizontal_lock, self.symmetry_lock)
        elif event == cv2.EVENT_LBUTTONUP:
            self.drag_index = -1

    def draw_points(self, canvas, x0, y0):
        for i, p in enumerate(self.points.astype(int)):
            cv2.circle(canvas, (x0 + p[0], y0 + p[1]), 8, (255, 255, 255), 2, cv2.LINE_AA)
            cv2.circle(canvas, (x0 + p[0], y0 + p[1]), 6, (166, 200, 43), -1, cv2.LINE_AA)
            cv2.putText(
                canvas,
                POINT_NAMES[i],
                (x0 + p[0] + 6, y0 + p[1] - 6),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.5,
                (0, 0, 0),
                1,
                cv2.LINE_AA,
            )

        order = [IDX["TL"], IDX["TM"], IDX["TR"], IDX["RM"], IDX["BR"], IDX["BM"], IDX["BL"], IDX["LM"], IDX["TL"]]
        for a, b in zip(order[:-1], order[1:]):
            pa = self.points[a].astype(int)
            pb = self.points[b].astype(int)
            cv2.line(canvas, (x0 + pa[0], y0 + pa[1]), (x0 + pb[0], y0 + pb[1]), (166, 200, 43), 2, cv2.LINE_AA)

    def render(self):
        knew = build_knew(self.k, self.knew_scale)
        und = undistort_image(self.image, self.k, self.d, knew)
        pts_und, quad, hmat, (rect_w, rect_h) = solve_rectification(
            self.points, self.k, self.d, knew, self.top_corner_user_blend
        )

        tm_ref = pts_und[IDX["TM"]].reshape(1, 1, 2)
        tm_rect = cv2.perspectiveTransform(tm_ref, hmat).reshape(2).astype(np.float32)

        rectified_full, rectified_crop, valid_after, crop_box = rectify_from_raw_with_tm_crop(
            self.image,
            hmat,
            (rect_w, rect_h),
            knew,
            self.k,
            self.d,
            tm_rect_y=float(tm_rect[1]),
            top_margin=10,
            pts_und=pts_und,
            top_corner_user_blend=self.top_corner_user_blend,
        )

        rect_preview = cv2.resize(rectified_crop, (PANEL_W, PANEL_H), interpolation=cv2.INTER_LINEAR)

        canvas = np.full(
            (HEADER_H + PANEL_H + FOOTER_H, PANEL_W * 3 + PANEL_GAP * 2, 3),
            245,
            dtype=np.uint8,
        )

        raw_x = 0
        und_x = PANEL_W + PANEL_GAP
        rect_x = PANEL_W * 2 + PANEL_GAP * 2
        y0 = HEADER_H

        canvas[y0:y0 + PANEL_H, raw_x:raw_x + PANEL_W] = self.image
        canvas[y0:y0 + PANEL_H, und_x:und_x + PANEL_W] = und
        canvas[y0:y0 + PANEL_H, rect_x:rect_x + PANEL_W] = rect_preview

        titles = ["Raw Calibration", "Undistorted Reference", "Rectified Preview"]
        for i, t in enumerate(titles):
            cv2.putText(
                canvas,
                t,
                (i * (PANEL_W + PANEL_GAP), 30),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.9,
                (0, 0, 0),
                2,
                cv2.LINE_AA,
            )

        self.draw_points(canvas, raw_x, y0)

        for i, name in enumerate(POINT_NAMES):
            p = pts_und[i].astype(int)
            p[0] = clamp(p[0], 0, PANEL_W - 1)
            p[1] = clamp(p[1], 0, PANEL_H - 1)
            cv2.circle(canvas, (und_x + p[0], y0 + p[1]), 6, (255, 255, 255), 2, cv2.LINE_AA)
            cv2.circle(canvas, (und_x + p[0], y0 + p[1]), 4, (102, 103, 251), -1, cv2.LINE_AA)
            cv2.putText(
                canvas,
                name,
                (und_x + p[0] + 6, y0 + p[1] - 6),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.45,
                (35, 45, 90),
                1,
                cv2.LINE_AA,
            )

        quad_int = quad.astype(int)
        for a, b in zip(range(4), [1, 2, 3, 0]):
            pa = quad_int[a]
            pb = quad_int[b]
            cv2.line(canvas, (und_x + pa[0], y0 + pa[1]), (und_x + pb[0], y0 + pb[1]), (0, 180, 255), 2, cv2.LINE_AA)

        tm_mid = pts_und[IDX["TM"]].astype(int)
        cv2.circle(canvas, (und_x + tm_mid[0], y0 + tm_mid[1]), 10, (0, 0, 255), 2, cv2.LINE_AA)

        x, y, cw, ch, ratio = crop_box
        footer = (
            f"{os.path.basename(self.image_paths[self.image_index])} | "
            f"Knew={self.knew_scale:.2f} | TopWeak={self.top_corner_user_blend:.2f} | "
            f"HLock={'On' if self.horizontal_lock else 'Off'} | "
            f"SLock={'On' if self.symmetry_lock else 'Off'} | "
            f"Crop=({x},{y},{cw},{ch}) | Valid={ratio:.3f} | "
            "Drag on raw panel, [ / ] switch image, R reset, S save, H/Y toggle"
        )
        cv2.putText(
            canvas,
            footer,
            (8, HEADER_H + PANEL_H + 30),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.52,
            (60, 60, 60),
            1,
            cv2.LINE_AA,
        )

        return canvas

    def run(self):
        cv2.namedWindow(WINDOW_NAME, cv2.WINDOW_NORMAL)
        cv2.resizeWindow(WINDOW_NAME, PANEL_W * 3 + PANEL_GAP * 2, HEADER_H + PANEL_H + FOOTER_H)
        cv2.setMouseCallback(WINDOW_NAME, self.mouse)

        cv2.createTrackbar("Knew x100", WINDOW_NAME, int(round(self.knew_scale * 100)), 100, lambda _v: None)
        cv2.createTrackbar("TopWeak x100", WINDOW_NAME, int(round(self.top_corner_user_blend * 100)), 50, lambda _v: None)

        while True:
            self.knew_scale = max(0.20, cv2.getTrackbarPos("Knew x100", WINDOW_NAME) / 100.0)
            self.top_corner_user_blend = cv2.getTrackbarPos("TopWeak x100", WINDOW_NAME) / 100.0

            frame = self.render()
            cv2.imshow(WINDOW_NAME, frame)
            key = cv2.waitKey(20) & 0xFF

            if key in (27, ord("q")):
                break
            elif key == ord("s"):
                self.save_points()
            elif key == ord("r"):
                self.reset_points()
            elif key == ord("["):
                self.image_index = (self.image_index - 1) % len(self.image_paths)
                self.load_image()
            elif key == ord("]"):
                self.image_index = (self.image_index + 1) % len(self.image_paths)
                self.load_image()
            elif key == ord("h"):
                self.horizontal_lock = not self.horizontal_lock
                self.points = constrain_points(self.points, self.horizontal_lock, self.symmetry_lock)
            elif key == ord("y"):
                self.symmetry_lock = not self.symmetry_lock
                self.points = constrain_points(self.points, self.horizontal_lock, self.symmetry_lock)

        cv2.destroyAllWindows()


if __name__ == "__main__":
    CalibrationApp().run()
