# T23/C3 校正算法详解

## 1. 文档目的

这份文档专门说明当前 `T23 + ESP32-C3` 方案里的电视边框校正算法，目标是回答 4 个问题：

- 当前到底是几何上怎么拉正的
- `C3` 网页、`T23` 校正图、后续灯带取色之间是什么关系
- 关键函数分别负责什么
- 这套实现为什么最终能和 Python 调参工具对齐

当前实现对应的主要代码：

- [T23 校正主实现](/home/kuan/T23-C3-Project/t23_rebuild/app/isp_bridge/src/main.c)
- [C3 网页交互与桥接](/home/kuan/T23-C3-Project/c3_rebuild/main/main.c)
- [共享 8 点数据结构](/home/kuan/T23-C3-Project/t23_c3_shared/include/t23_border_pipeline.h)

## 2. 当前算法结论

当前版本不是旧的 10 点 Coons Patch 曲面插值，而是：

- 基于固定鱼眼标定参数的 8 点校正模型
- 在鱼眼去畸变后的坐标系里拟合四条边
- 再通过单个 `homography` 拉成规则矩形
- 最后在规则矩形里选一个满足约束的 `16:9` crop

这套实现的特点是：

- 页面交互点只有 8 个，拖点复杂度低
- 真实校正结果由 T23 内部完成，不是网页端假预览
- 最终灯带取色和网页 `Rectified Preview` 共用同一套 rectification model 和 crop

## 3. 8 点数据模型

当前共享点位定义在 [t23_border_pipeline.h](/home/kuan/T23-C3-Project/t23_c3_shared/include/t23_border_pipeline.h)：

- `TL` 左上角
- `TM` 上边中点
- `TR` 右上角
- `RM` 右边中点
- `BR` 右下角
- `BM` 下边中点
- `BL` 左下角
- `LM` 左边中点

对应结构体：

- `t23_border_point_t`：单个点的像素坐标
- `t23_border_calibration_t`：图像宽高 + 8 个点

网页、C3、T23 都用这套同名点位，不再存在旧版 `TML/TMR`。

## 4. 端到端链路

一次真实校正预览的完整路径如下：

1. 网页加载原始校准图：`loadCalibrationSnapshot()`
2. 用户拖动 8 个点，网页本地做约束：`applyHorizontalConstraint()`、`applySymmetryConstraint()`
3. 点击确认后，网页把 8 点通过 `/api/calibration/set` 交给 C3：`buildCalibrationQuery()`、`saveCalibration()`
4. C3 通过 UART 发送 `CAL SET`
5. 网页再请求 `/api/calibration/rectified`
6. C3 通过 UART 发送 `CAL SNAP`
7. T23 抓当前 JPEG：`handle_cal_snap()`
8. T23 在本地完成解码、校正、裁剪、重编码：`rectify_jpeg_from_calibration()`
9. 校正 JPEG 通过 SPI 回到 C3，再经 HTTP 返回浏览器

关键点：

- `C3` 负责交互、传输、页面约束
- `T23` 负责真实几何校正
- 网页右侧看到的结果，就是 T23 输出的真实 rectified 图

## 5. T23 校正算法步骤

### 5.1 总入口

当前 T23 侧主入口函数：

- `handle_cal_snap()`：抓取当前 JPEG，调用校正，并把结果推回 C3
- `rectify_jpeg_from_calibration()`：完成“解码 -> 校正 -> 编码”
- `build_rectified_rgb_from_calibration()`：生成最终的 rectified `RGB888`

### 5.2 初始化 fixed fisheye 模型

函数：

- `init_fixed_fisheye_profile()`

职责：

- 装载固定鱼眼标定参数 `fx / fy / cx / cy / k1..k4`
- 构造当前使用的 `knew`
- 这里和 Python 对齐的一点很关键：只缩放 `fx / fy`，`cx / cy` 保持标定值不变

这一步决定了“原始鱼眼图 -> undistorted 坐标”的基础映射。

### 5.3 单点鱼眼去畸变

函数：

- `undistort_point_with_fisheye()`

职责：

- 把某个原图点映射到 undistorted 坐标
- 使用预生成的 OpenCV 有效性掩码 `fisheye_valid_mask_640x320.h`
- 对齐 Python/OpenCV 在鱼眼边缘“哪些点算无效”的判定

这一步直接解决了一个关键差异：

- Python 里 `TL/TR` 靠近鱼眼边缘时，经常会变成无效点
- 如果 T23 继续把这些点当有效点，`TL/TR` 一动，结果就会剧烈变化

现在 T23 的点有效性和 Python 已经对齐。

### 5.4 从 8 个点拟合电视四边

函数：

- `finalize_rectification_model_with_k1()`

职责：

- 对 8 个原始点做去畸变
- 用中下边点拟合规则四边
- 生成最终的 `quad` 和 `homography`

当前边界拟合规则：

- 底边：由 `BL / BM / BR` 拟合
- 左边：由 `BL / LM` 拟合
- 右边：由 `BR / RM` 拟合
- 顶边：不是直接用 `TL / TR`

顶边的真实做法和 Python 一致：

1. 先根据左右边外推两个预测顶角
   - `tl_pred = 2 * LM - BL`
   - `tr_pred = 2 * RM - BR`
2. 再把用户的 `TL / TR` 只按较小权重混进去
3. 最终再用这两个参考顶角拟合顶边

也就是说：

- `TL/TR` 不是主导顶边的点
- 它们只是一个弱参考
- 所以小范围移动 `TL/TR` 不应该让结果剧烈变化

### 5.5 生成 rectification model

函数：

- `build_rectification_model()`

职责：

- 构造完整的 `rectification_model_t`
- 优先走 fixed fisheye 主路径
- 计算 `TM` 经过 homography 后在 rectified 图中的位置 `tm_rect_y`
- 再调用 crop 逻辑得到最终 `crop_left / crop_top / crop_width / crop_height`

这里的 `tm_rect_y` 非常重要：

- 它不是顶边中点
- 而是 `TM` 这个用户点被映射到 rectified 图后的 `y`

后面的顶部安全约束都基于它。

## 6. Crop 为什么这样设计

### 6.1 先生成整张 rectified 图的有效区域

函数：

- `build_direct_valid_mask()`

职责：

- 对 rectified 画布上的每个像素，反查它是否能落回原始图像范围内
- 能落回去记为有效，否则记为无效

这张 `valid mask` 决定了：

- 哪些 crop 会带黑边
- 哪些 crop 是完全有效的

### 6.2 用积分图加速候选框评分

函数：

- `build_invalid_integral_image()`
- `count_invalid_pixels()`
- `score_centered_crop()`

职责：

- 先把无效像素构造成积分图
- 每个候选 `16:9` 框的无效像素数改成 O(1) 计算
- 避免 T23 在大范围搜索时因逐像素统计而导致 `HTTP 500`

这是当前版本很重要的一次工程修正：

- 几何策略没变
- 但性能从“每个候选框遍历整块 ROI”降到了常数时间统计

### 6.3 最终 16:9 crop 选择

函数：

- `find_centered_16x9_crop()`
- `finalize_rectification_crop()`

职责：

- 在 rectified 画布中找一个尽量居中的 `16:9` crop
- 同时满足顶部安全约束
- 优先选择满足有效率要求的最大候选框

当前关键约束：

- `safe_top = floor(tm_rect_y - 10)`
- crop 顶部不能高于 `safe_top`

当前实现还做了一个非常关键的修正：

- 当 `safe_top` 生效时，不能只在顶部附近做局部小范围搜索
- 否则虽然头顶不黑，但会把下身裁掉
- 现在会在可行的纵向范围内继续搜索，从而把底部空间尽量拿回来

这也是“顶部黑块消失，同时机器猫下身尽量保留”的根本原因。

## 7. 为什么现在能和 Python 对齐

这轮对齐里最关键的几件事是：

1. `knew` 只缩放 `fx / fy`，不重置 `cx / cy`
2. 顶边不是直接由 `TL / TR` 决定，而是由左右边外推 + 弱混合
3. `TL/TR` 在鱼眼边缘的无效判定和 OpenCV 对齐
4. `safe_top` 使用 `TM` 过 homography 后的 `tm_rect_y`
5. crop 搜索同时考虑 `16:9`、中心偏好、有效区域和 `safe_top`

因此现在的网页版本和 Python 调参工具，在视觉行为上已经对齐到同一套几何模型，而不是只是“看起来差不多”。

## 8. C3 网页侧函数职责

### 8.1 默认点与约束

函数：

- `makeDefaultCalibration()`
- `applyHorizontalConstraint()`
- `applySymmetryConstraint()`

职责：

- 生成和 Python 一致的默认 8 点梯形
- 对左右成对点施加水平约束
- 可选地施加左右对称约束

### 8.2 图像与保存

函数：

- `loadCalibrationSnapshot()`
- `loadRectifiedPreview()`
- `buildCalibrationQuery()`
- `saveCalibration()`
- `loadCalibration()`

职责：

- 拉取当前原始校准图
- 拉取 T23 返回的真实 rectified 预览
- 把当前 8 点编码成 HTTP 查询参数
- 负责“网页拖点 -> C3 API -> T23 保存/回显”这条链

### 8.3 镜头参数展示

函数：

- `refreshLensProfile()`

职责：

- 从 T23 读取当前固定鱼眼参数
- 在页面上展示 `fx / fy / cx / cy / k1 / k2 / scale`
- 方便判断当前校正链是不是跑在预期镜头配置上

## 9. 后续灯带取色为什么要复用这套结果

当前灯带取色不是另一套独立几何，而是继续建立在同一个 rectification model 上。

相关函数：

- `compute_average_rectified_patch()`
- `compute_border_blocks()`

职责：

- 基于 rectified crop 坐标系定义边框采样区域
- 逐块计算平均颜色
- 输出顺时针排列的边框块结果

这意味着：

- 网页上看到的 rectified 结果
- T23 内部边框色块的坐标系
- 后续灯带映射

三者必须严格共用同一套 crop 和几何模型。否则网页里“看起来对”，灯带上仍然会偏。

## 10. 关键函数速查表

### 10.1 T23

| 函数 | 作用 |
|---|---|
| `handle_cal_snap()` | 处理 `CAL SNAP`，抓图并返回真实校正 JPEG |
| `rectify_jpeg_from_calibration()` | 解码、校正、重编码总入口 |
| `build_rectified_rgb_from_calibration()` | 构造最终 rectified `RGB888` |
| `init_fixed_fisheye_profile()` | 初始化固定鱼眼模型参数 |
| `undistort_point_with_fisheye()` | 对单个点执行鱼眼去畸变 |
| `finalize_rectification_model_with_k1()` | 从 8 个点拟合 quad 与 homography |
| `build_rectification_model()` | 生成完整 rectification model，并计算 crop |
| `build_direct_valid_mask()` | 计算整张 rectified 画布的有效区域 |
| `build_invalid_integral_image()` | 为 crop 搜索建立无效像素积分图 |
| `find_centered_16x9_crop()` | 搜索满足约束的最佳 16:9 crop |
| `finalize_rectification_crop()` | 汇总 crop 约束并写回 model |
| `compute_average_rectified_patch()` | 在 rectified crop 上统计单块平均颜色 |
| `compute_border_blocks()` | 按布局输出边框块颜色 |

### 10.2 C3

| 函数 | 作用 |
|---|---|
| `makeDefaultCalibration()` | 生成默认 8 点 |
| `applyHorizontalConstraint()` | 施加水平锁约束 |
| `applySymmetryConstraint()` | 施加对称锁约束 |
| `loadCalibrationSnapshot()` | 拉取原始校准图 |
| `loadRectifiedPreview()` | 拉取 T23 的真实校正预览 |
| `buildCalibrationQuery()` | 打包当前 8 点参数 |
| `saveCalibration()` | 提交 8 点到 T23 并刷新预览 |
| `loadCalibration()` | 读取已保存点位 |
| `refreshLensProfile()` | 读取并展示固定鱼眼参数 |

## 11. 现阶段边界

当前版本已经稳定解决：

- Python 与网页校正逻辑不一致
- `TL/TR` 过度敏感
- 顶部黑块
- 在 T23 上搜索 crop 造成的性能退化

当前仍然刻意没有做的事：

- 不引入在线镜头参数拟合
- 不把网页拖点直接当作最终四角透视
- 不让网页预览和灯带取色走两套不同几何

这样做的目的，是让“校正预览”和“后续真实灯带结果”保持同一坐标系和同一套工程实现。
