import cv2
from cv2 import aruco

# 创建字典
aruco_dict = aruco.getPredefinedDictionary(aruco.DICT_5X5_1000)

# 创建 ChArUco 板
board = aruco.CharucoBoard(
    (7, 5),      # 棋盘格数量 (列, 行)
    0.03,        # 方格尺寸（米）
    0.02,        # ArUco 标记尺寸（米）
    aruco_dict
)

# 保存为图片
img = board.generateImage((2000, 1500))
cv2.imwrite("charuco_board.png", img)