# python_calibration

这个目录是对本地 Windows 路径 `C:\Code\T23-C3-Project\Python` 的仓库内镜像，方便把桌面 Python 调参工具、测试图片和标定文件一起纳入 Git 追踪。

当前已同步的内容包括：

- `distortion calibration.py`
- `lens calibration.py`
- `ChArUco.py`
- `fisheye_charuco_calibration.npz`
- `charuco_board.png`
- `Images/`
- `Images.zip`

为了避免仓库变脏，下面两类本地噪音文件没有一起提交：

- IDE 配置目录：`.idea/`
- 运行缓存目录：`__pycache__/`

如果后续还需要把这个目录完全恢复到 Windows 本地工具形态，直接以这个目录为基础再补回 IDE/缓存文件即可；算法和素材本身已经都在这里了。

另外：

- [python_rectification_reference.py](/home/kuan/T23-C3-Project/pc_tuner/python_rectification_reference.py) 是从这些脚本里抽出来、便于复用和对照 T23 固件的核心参考实现
- 这份 `python_calibration/` 则更接近你平时直接运行的完整 Python 文件夹
