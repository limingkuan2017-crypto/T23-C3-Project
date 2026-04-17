# Python/T23 算法同步说明

这份说明专门记录桌面 Python 调参工具与当前 `T23` 固件之间，已经统一的那部分校正算法实现。

当前 GitHub 仓库里的 Python 参考代码在：

- [pc_tuner/python_rectification_reference.py](/home/kuan/T23-C3-Project/pc_tuner/python_rectification_reference.py)

本地 Windows 调参脚本在：

- `C:\Code\T23-C3-Project\Python\distortion calibration.py`

两边现在保持一致的核心点是：

1. `TM` 先过 `homography` 得到 `tm_rect_y`
2. `safe_top = floor(tm_rect_y - 10)`
3. `safe_top` 不是事后再把 crop 往下推，而是直接带进 `16:9 crop` 搜索
4. crop 优先选择 `100% valid` 的候选框
5. 当 `safe_top` 生效时，纵向搜索会覆盖完整可行范围，避免“头部保住了、下身又被切掉”
6. 有效像素统计统一改成“invalid integral image”，避免逐像素统计带来的性能问题

## 函数职责

### `build_invalid_integral_image(valid_mask)`

- 输入 rectified 画布上的 `valid mask`
- 输出“无效像素积分图”
- 作用是把任意 ROI 的无效像素数计算，从逐像素遍历降到 `O(1)`

### `count_invalid_pixels(invalid_integral, x, y, w, h)`

- 从积分图里读取指定候选框的无效像素总数
- 这是 crop 评分的基础统计函数

### `score_centered_crop(invalid_integral, x, y, w, h, desired_center)`

- 计算一个候选 crop 的评分
- 评分由三部分组成：
  - 面积越大越好
  - 有效像素比例越高越好
  - 离目标中心越近越好

### `find_centered_16x9_crop(valid_mask, desired_center, min_top, min_valid_ratio)`

- 在 rectified 画布里搜索最终使用的 `16:9` crop
- `min_top` 对应 `safe_top`
- 先尝试找到满足 `min_valid_ratio` 的最大候选框
- 如果 `safe_top` 生效，会把纵向搜索范围放大到完整可行区间，而不是只在中心附近小范围试探

### `rectify_from_raw_with_tm_crop(raw_img, hmat, out_size, knew, k, d, tm_rect_y, top_margin)`

- 这是 Python 端与 T23 对齐后的总入口
- 先生成整张 `rectified_full`
- 再基于 `TM -> safe_top` 和 `valid mask` 选出最终 crop
- 返回：
  - `rectified_full`
  - `rectified_crop`
  - `valid_crop`
  - `(x, y, w, h, valid_ratio)` 裁剪信息

## 这次统一修掉的历史差异

之前 Python 端存在这条旧逻辑：

1. 先找一个 `min_valid_ratio=0.95` 的局部居中 crop
2. 再在搜索完成后，用 `safe_top` 把结果强制往下推

这会带来两个问题：

- 顶部黑块和底部裁切之间会互相拉扯
- 和当前 T23 固件的结果不完全一致

现在已经改成与 T23 相同的逻辑：

- 搜索时直接考虑 `safe_top`
- 优先 `100% valid`
- 用积分图加速候选框评分

这样桌面 Python 结果和网页/T23 结果会更稳定地对齐。
