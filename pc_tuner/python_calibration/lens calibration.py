import cv2
import glob
import os
import numpy as np

IMAGE_DIR = r"C:\Code\T23-C3-Project\Python\Images"
image_paths = glob.glob(os.path.join(IMAGE_DIR, "*.jpg"))

aruco_dict = cv2.aruco.getPredefinedDictionary(cv2.aruco.DICT_5X5_1000)

# 这里一定要和你生成/打印的板子一致
squares_x = 7
squares_y = 5
square_length = 0.03
marker_length = 0.02

board = cv2.aruco.CharucoBoard(
    (squares_x, squares_y),
    square_length,
    marker_length,
    aruco_dict
)

objpoints = []
imgpoints = []
image_size = None

for img_path in image_paths:
    img = cv2.imread(img_path)
    if img is None:
        print(f"Failed to read: {img_path}")
        continue

    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)

    if image_size is None:
        image_size = gray.shape[::-1]

    corners, ids, _ = cv2.aruco.detectMarkers(gray, aruco_dict)

    if ids is None or len(ids) == 0:
        print(f"{os.path.basename(img_path)}: No ArUco markers detected")
        continue

    retval, charuco_corners, charuco_ids = cv2.aruco.interpolateCornersCharuco(
        corners, ids, gray, board
    )

    if retval is None or charuco_corners is None or charuco_ids is None:
        print(f"{os.path.basename(img_path)}: ChArUco interpolation failed")
        continue

    if len(charuco_ids) < 8:
        print(f"{os.path.basename(img_path)}: Not enough ChArUco corners ({len(charuco_ids)})")
        continue

    # 关键：根据 charuco_ids 取对应的 3D 世界点
    obj = board.getChessboardCorners()[charuco_ids.flatten()]
    imgp = charuco_corners.reshape(-1, 2)

    objpoints.append(obj.reshape(1, -1, 3).astype(np.float32))
    imgpoints.append(imgp.reshape(1, -1, 2).astype(np.float32))

    print(f"{os.path.basename(img_path)}: markers={len(ids)}, charuco_corners={len(charuco_ids)}")

print(f"\nValid images for calibration: {len(objpoints)}")

if len(objpoints) == 0:
    raise RuntimeError("No valid calibration images found.")

K = np.zeros((3, 3))
D = np.zeros((4, 1))

flags = (
    cv2.fisheye.CALIB_RECOMPUTE_EXTRINSIC +
    cv2.fisheye.CALIB_CHECK_COND +
    cv2.fisheye.CALIB_FIX_SKEW
)

criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 100, 1e-6)

rms, K, D, rvecs, tvecs = cv2.fisheye.calibrate(
    objpoints,
    imgpoints,
    image_size,
    K,
    D,
    None,
    None,
    flags=flags,
    criteria=criteria
)

# for img_path in image_paths:
#     img = cv2.imread(img_path)
#     if img is None:
#         continue
#     print(os.path.basename(img_path), img.shape[:2])  # (h, w)

print("\n=== Calibration Results ===")
print("RMS Error:", rms)
print("Camera Matrix K:\n", K)
print("Distortion D:\n", D)

np.savez("fisheye_charuco_calibration.npz", K=K, D=D, image_size=image_size)
print("\nSaved to fisheye_charuco_calibration.npz")