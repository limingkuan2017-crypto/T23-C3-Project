# Python/T23 算法同步说明

这份说明记录桌面 Python 调参工具与当前 `T23` 固件之间，已经统一到同一套实现的校正逻辑。

当前仓库里的 Python 代码在：

- [pc_tuner/python_calibration/distortion calibration.py](/home/kuan/T23-C3-Project/pc_tuner/python_calibration/distortion%20calibration.py)
- [pc_tuner/python_rectification_reference.py](/home/kuan/T23-C3-Project/pc_tuner/python_rectification_reference.py)

本地 Windows 调参脚本在：

- `C:\Code\T23-C3-Project\Python\distortion calibration.py`

## 当前统一后的结论

两边现在保持一致的核心点是：

1. `TL/TR` 仍然只是顶边的弱参考，顶边主体由 `BL/LM`、`BR/RM` 外推出来。
2. `knew` 仍然只缩放 `fx/fy`，不重置 `cx/cy`。
3. rectified 基础画布尺寸不再强制压成 `16:9`，而是按当前拟合出的 `quad` 宽高自适应估算。
4. 最终输出不再为了 `16:9` 主动裁掉上下内容。
5. crop 选择改成“内容完整优先，再保证无黑边，最后才兼顾比例自然”。
6. Python 和 T23 都用无效像素积分图做候选框判定，行为一致，性能也一致。

## 函数职责

### `compute_rectified_size_from_quad(quad)`

- 根据当前 `quad` 的四边长度估算 rectified 画布尺寸
- 不再固定输出 `16:9`
- 只在最小尺寸和最大尺寸约束内做缩放

### `build_invalid_integral_image(valid_mask)`

- 输入 rectified 画布上的 `valid mask`
- 输出“无效像素积分图”
- 让任意候选矩形的无效像素统计降为 `O(1)`

### `rectified_rect_is_valid(invalid_integral, left, top, right, bottom)`

- 判断一个 rectified 矩形是否完全有效
- 这是“不要黑边”的最基本判定函数

### `compute_required_content_bounds(pts_und, hmat, out_size, top_corner_user_blend)`

- 把“用户框选出的电视内容”投影到 rectified 坐标系
- 用 `TM / LM / RM / BL / BM / BR` 以及按当前顶边模型外推的 `TL/TR` 参考点
- 输出一个“必须保留内容”的最小外接框

### `find_nearest_valid_seed(valid_mask, center_x, center_y)`

- 当“必须保留内容框”本身落到了无效区时
- 从其中心附近找到最近的有效起点
- 避免整套 crop 因边缘个别无效点直接失败

### `expand_valid_crop_greedy(invalid_integral, valid_mask, desired_aspect, left, top, right, bottom)`

- 从当前有效种子框开始，按左、上、右、下以及组合方向逐步扩展
- 只接受完全有效的新矩形
- 在“内容已保留”的前提下尽量把范围做大
- `desired_aspect` 只作为平手时的弱偏好，不再主导裁剪

### `rectify_from_raw_with_tm_crop(raw_img, hmat, out_size, knew, k, d, tm_rect_y, top_margin, pts_und, top_corner_user_blend)`

- 保留了原函数名，方便 Python UI 侧少改调用关系
- 实际行为已经不再依赖 `TM -> safe_top -> 16:9 crop`
- 现在流程是：
  1. 生成整张 `rectified_full`
  2. 计算 `valid mask`
  3. 投影“必须保留内容框”
  4. 以它为中心扩展出最大的全有效矩形
  5. 返回最终 `rectified_crop`

## 这次统一修掉的历史差异

之前 Python 和较早的 T23 版本都带过这类旧行为：

1. 先生成 rectified 图
2. 再找一个看起来更舒服的 `16:9` crop
3. 为了避免黑边，会进一步压缩顶部或底部

这样做的问题是：

- 摄像头俯仰一变化，最终显示范围也会明显变化
- 明明用户已经把电视边框框准了，输出内容还是可能被切掉
- “画面完整”和“没有黑边”之间经常互相拉扯

现在已经统一改成：

- 保持原有 8 点拟合和弱顶角模型不动
- 只改最后的内容选择策略
- 输出尽量完整保留用户框选出的电视内容
- 同时尽量不带黑边
- 不再为固定 `16:9` 比例牺牲上下内容

这也是当前 Python 调参窗口与 T23 网页预览能够重新对齐的关键原因。
