# 新电脑落地手册

## 目的

这份文档是给“第一次把这个仓库拉到一台新电脑上”的人用的。

目标不是解释所有代码，而是帮助你最快完成这几件事：

1. 把仓库放到正确位置
2. 把外部依赖放到正确位置
3. 检查目录是否完整
4. 构建 T23 侧程序
5. 构建 C3 侧程序

## 一、推荐环境

当前项目默认按下面的环境验证过：

- Windows + WSL2 Ubuntu
- T23 构建在 Ubuntu / WSL 中完成
- ESP32-C3 构建在 Ubuntu / WSL 中完成
- Markdown 文档可在 Windows 阅读器中直接打开 `\\wsl.localhost\...`

## 二、克隆仓库

在 Ubuntu / WSL 中执行：

```sh
cd ~
git clone git@github.com:limingkuan2017-crypto/T23-C3-Project.git
cd T23-C3-Project
```

## 三、准备第三方依赖

这个仓库不会提交大体积或闭源依赖，所以你需要自己把它们放到：

```text
T23-C3-Project/
└─ third_party/
   ├─ ingenic_t23_sdk/
   └─ vendor_reference/
```

### 1. Ingenic T23 SDK

你需要把官方 SDK 解压或复制到：

```text
third_party/ingenic_t23_sdk
```

确保下面这些路径存在：

```text
third_party/ingenic_t23_sdk/sdk/include
third_party/ingenic_t23_sdk/sdk/lib/uclibc
```

### 2. 供应商参考工程

你需要把供应商参考工程放到：

```text
third_party/vendor_reference
```

确保下面这些路径存在：

```text
third_party/vendor_reference/build/images
third_party/vendor_reference/build/toolchain
```

## 四、如果你本机已经有旧目录

如果旧电脑上的目录还在，比如：

- `/home/kuan/ingenic_t23_sdk/Ingenic-SDK-T23-1.1.2-20240204-en`
- `/home/kuan/hj_t23_demo`

你可以先用软链接方式快速接入：

```sh
cd ~/T23-C3-Project
ln -sfn /home/kuan/ingenic_t23_sdk/Ingenic-SDK-T23-1.1.2-20240204-en third_party/ingenic_t23_sdk
ln -sfn /home/kuan/hj_t23_demo third_party/vendor_reference
```

这种方式适合过渡期，后面也可以改成真正复制到仓库旁边的目录。

## 五、检查工作区

仓库根目录下运行：

```sh
cd ~/T23-C3-Project
./scripts/check_workspace.sh
```

如果输出全部是 `[ok]`，说明目录结构已经满足当前工程需要。

也可以直接使用顶层引导脚本：

```sh
./scripts/bootstrap.sh --check
```

## 六、构建 T23

### 只构建诊断程序

```sh
cd ~/T23-C3-Project
./scripts/bootstrap.sh --build-t23
```

或者单独执行：

```sh
cd ~/T23-C3-Project/t23_rebuild
./scripts/build_camera.sh
cd app/spi_diag
make clean all
```

成功后主要产物在：

```text
t23_rebuild/output/t23_camera_diag
t23_rebuild/output/t23_spi_diag
```

### 生成 T23 烧录包

```sh
cd ~/T23-C3-Project
./scripts/bootstrap.sh --package-t23
```

成功后主要产物在：

```text
t23_rebuild/_release/image_t23/
```

## 七、构建 C3

### 1. 准备 ESP-IDF

当前工程优先使用：

```text
$IDF_PATH
```

如果没有显式设置，则会尝试：

```text
~/.espressif/v5.4.3/esp-idf
```

### 2. 构建

```sh
cd ~/T23-C3-Project
./scripts/bootstrap.sh --build-c3
```

或者单独执行：

```sh
cd ~/T23-C3-Project/c3_rebuild
./scripts/idf_build.sh
```

成功后主要产物在：

```text
c3_rebuild/build/c3_rebuild.bin
```

## 八、最快的一条命令

如果你的依赖都已放好，想整套检查并构建：

```sh
cd ~/T23-C3-Project
./scripts/bootstrap.sh --all
```

这会执行：

1. 工作区检查
2. T23 诊断程序构建
3. T23 烧录包打包
4. C3 固件构建

## 九、最常见的问题

### 1. `vendor_reference` 或 `ingenic_t23_sdk` 找不到

先跑：

```sh
./scripts/check_workspace.sh
```

通常是第三方依赖没放到 `third_party/` 里，或者软链接目标不存在。

### 2. T23 可以编译，C3 不行

通常优先检查：

- `IDF_PATH` 是否正确
- `~/.espressif/v5.4.3/esp-idf` 是否存在
- ESP-IDF 的 Python 环境是否已经安装完成

### 3. C3 能构建，但不能刷机

这通常不是仓库结构问题，而是：

- 串口映射问题
- 自动下载模式控制线问题
- 板子没有进入下载模式

这部分优先看：

- `c3_rebuild/scripts/idf_flash.sh`
- `c3_rebuild/scripts/manual_flash.sh`

## 十、建议的新手阅读顺序

如果你已经把环境搭好，下一步推荐按下面顺序读文档：

1. `README.md`
2. `t23_rebuild/docs/t23_runtime_flow.md`
3. `t23_rebuild/docs/t23_function_guide_zh.md`
4. `t23_rebuild/docs/t23_learning_path_zh.md`
5. `t23_c3_shared/docs/system_initialization_flow_zh.md`
6. `t23_c3_shared/docs/project_handover.md`
