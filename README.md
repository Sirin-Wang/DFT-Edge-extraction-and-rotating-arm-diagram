# ProjectFourier (DFT-Edge-extraction-and-rotating-arm-diagram)

ProjectFourier 用于从图片中提取线稿组件，把线稿转成 SVG，并生成傅里叶旋臂绘制动画数据。项目包含本地网页工具，可以导入图片、提取线稿、预运算 DFT，并查看 `Sequential`、`Simultaneous`、`Full SVG` 等动画效果。

主要流程：

1. 读取图片并做增强处理。
2. 提取主体/引导区域。
3. 用 `XDoG` 等方法提取线稿。
4. 通过 `potrace` 把线稿转成 SVG。
5. 拆分连通组件并生成 component SVG。
6. 对 component 或整张 SVG 做 DFT 预运算。
7. 在网页中播放旋臂动画。

## 程序内部图片处理流程

图片提取入口是 `ProjectFourier.exe`，核心逻辑在 `main_v2.cpp` 的 `processImageV2`。它把一张输入图转换成两类线稿通道：

- `XDoG_Guide`：更偏主体和关键结构的线稿。
- `XDoG_Support`：在主体支持区域内补充更多可用线条。

内部处理大致分为以下阶段。

### 1. 读取与尺寸归一化

程序先用 OpenCV 读取原图：

```cpp
imread(imagePath, IMREAD_UNCHANGED)
```

然后根据 `V2Options` 做两级缩放：

- `maxProcessingDim`：限制外部处理尺寸，默认约 `1180`，避免超大图直接参与后续计算。
- `internalScale`：内部处理放大倍率，默认约 `1.30`，用于让线稿提取有更多细节。

如果图片短边低于 `lowResMinDim`，且没有关闭低分辨率增强，程序会额外计算 `lowResEnhanceScale`，再调用：

```cpp
enhanceLowResolutionForXDoGV2(workSrc)
```

这一步用于让低分辨率图片在 XDoG 和边缘检测前更稳定。

### 2. 构建角色/主体引导区域

程序调用：

```cpp
buildCharacterGuideBundleV2(workSrc, Mat())
```

生成 `V2CharacterGuideBundle`，其中包含：

- `subjectMask`：主体区域。
- `headGuide`：头部引导区。
- `faceGuide`：脸部引导区。
- `eyeGuide`：眼部引导区。
- `upperBodyGuide`：上半身引导区。
- `lowerBodyGuide`：下半身引导区。
- `characterGuide`：合并后的主体关键引导区。
- `characterSupport`：后续补线用的支持区域。

主体区域优先利用透明通道；没有透明通道时，会用边框背景模型和中心先验估计前景。随后再根据主体包围盒、肤色/亮度/饱和度、暗部和边缘特征估计头部、脸部、眼部和身体区域。

这些 guide 不直接作为最终线稿，而是用于限制后续 XDoG 和细节线条的保留范围，减少背景噪声。

### 3. 生成候选线稿

程序调用：

```cpp
buildLineMaskV2(
    workSrc,
    characterGuides.characterGuide,
    &xdogMask,
    &strongSupport,
    &fineDetail,
    &filledRegions,
    &regionOutlines,
    &structuralSupport,
    tuneXdogDetails
)
```

这一阶段会综合多种线条来源：

- LAB 亮度边缘。
- LAB 色度边缘。
- HSV 饱和区域。
- XDoG 线稿候选。
- blackhat 暗线检测。
- 局部细节检测。
- 上半区颜色边界。
- 填充色块的外轮廓。

其中 XDoG 候选由 `buildXDoGLineMask` 生成，基本思路是对亮度图做两级高斯模糊，计算 DoG 差分，再阈值化成二值线稿。程序还会用不同尺度的边缘和暗线结果补充细线。

### 4. 过滤噪声与色块

候选线稿不会直接输出。程序会继续做多轮过滤：

- `filterLineComponentsV2`：按连通组件面积、形状、支持区域重叠比例过滤。
- `removeSmallComponents`：删除小面积噪声。
- `removeFilledColorBlocksV2`：识别大块填充区域，只保留边界。
- `hollowThickColorCoresV2`：把过厚的色块核心空心化，减少实心块。
- `outlineFilledRegionComponents`：把填充区域转换成轮廓。
- `suppressBackgroundNoiseComponents`：利用结构支持区和 XDoG 重叠关系压制背景孤立噪声。
- `keepTopComponentsPerTileV2`：对高密度区域按 tile 限制保留数量，避免局部线条过密。

过滤后的结果是 `lineMask`。随后会再调用：

```cpp
cleanBinaryMaskForContours(lineMask)
```

得到更适合轮廓导出的二值线稿。

### 5. 分成 Guide 与 Support 两个通道

程序不会只输出一个线稿，而是生成两个通道。

`XDoG_Guide`：

```cpp
bitwise_and(xdogMask, characterGuides.characterGuide, xdogGuide)
```

它主要保留主体关键引导区内的 XDoG 线条，之后还会经过色块清理和厚色块空心化。

`XDoG_Support`：

```cpp
bitwise_and(xdogMask, characterGuides.characterSupport, xdogSupport)
xdogSupport |= recoverLineLikeSupportV2(lineMask, xdogSupport, characterGuides.characterSupport)
```

它在支持区域内保留更多线条，并从 `lineMask` 中恢复接近支持区的线状组件，用于补充 Guide 可能漏掉的细节。

最终两个通道都会再次清理小组件，输出为稳定的二值 mask。

### 6. 可选输出中间图

如果命令行带 `--intermediate` 或 `--save-intermediate`，程序会保存中间结果，方便检查是哪一步导致线稿偏差。

输出目录：

```text
results_v2\<image_name>\intermediate\
results_v2\<image_name>\art_intermediate_case.png
```

中间图包括：

- `input_resized`
- `working_enhanced`
- `subject_mask`
- `character_guide`
- `xdog_candidate`
- `edge_support`
- `fine_detail`
- `filled_regions`
- `region_outlines`
- `structural_support`
- `line_mask_clean`
- `support_mask`
- `xdog_guide_final`
- `xdog_support_final`
- `guide_overlay`
- `support_overlay`

如果不启用 `--intermediate`，程序会删除当前图片旧的中间图，避免输出目录持续膨胀。

### 7. 导出 BMP/SVG 与 component SVG

最终 mask 会通过：

```cpp
writeMaskOutputsV2(outputDir, "XDoG_Guide", xdogGuide, resized.size())
writeMaskOutputsV2(outputDir, "XDoG_Support", xdogSupport, resized.size())
```

输出完整通道：

```text
results_v2\<image_name>\XDoG_Guide.bmp
results_v2\<image_name>\XDoG_Guide.svg
results_v2\<image_name>\XDoG_Support.bmp
results_v2\<image_name>\XDoG_Support.svg
```

SVG 转换由 `potrace` 完成。程序会先把二值 mask 写成临时 PBM，再调用：

```cmd
potrace <temp.pbm> -s -i
```

随后程序调用：

```cpp
writeConnectedComponentSVGsV2(xdogGuide, componentDir, "XDoG_Guide", resized.size())
writeConnectedComponentSVGsV2(xdogSupport, componentDir, "XDoG_Support", resized.size())
```

它会对每个通道做连通组件分析，把每个 component 单独转成 SVG：

```text
results_v2\<image_name>\comp\XDoG_Guide_0000.svg
results_v2\<image_name>\comp\XDoG_Guide_0001.svg
results_v2\<image_name>\comp\XDoG_Support_0000.svg
...
```

这些 component SVG 是后续 DFT 预运算的主要输入。

### 8. DFT 预运算与网页播放

提取完成后，`DftPrecompute.exe` 会读取：

```text
results_v2\<image_name>\comp\
```

然后根据 `dft_scene_params.txt` 和网页传入参数，对 component 或整张 SVG 采样并计算傅里叶系数。主要输出：

```text
results_v2\<image_name>\dft_data\
results_v2\<image_name>\FourierCoefficient\
```

网页 `Scene` 模式读取这些预运算 JSON 数据，播放 `Sequential`、`Simultaneous` 或 `Full SVG` 动画。

## 快速开始

### 使用预编译包

如果只是使用工具，不需要在目标电脑安装 Visual Studio，也不需要重新编译。

1. 下载或复制 `dist\ProjectFourier-win-x64.zip`。
2. 解压整个压缩包。
3. 进入解压后的 `ProjectFourier-win-x64` 文件夹。
4. 双击运行：

```cmd
start_dft_viewer.cmd
```

5. 保持弹出的 PowerShell 窗口不要关闭。
6. 浏览器通常会自动打开：

```text
http://127.0.0.1:5174/dft_viewer.html
```

如果 `5174` 被占用，服务会自动尝试后续端口，例如 `5175`。请以 PowerShell 窗口实际打印的地址为准。

预编译包仍然需要目标电脑安装 `Node.js LTS`，因为网页工具由本地 Node.js 服务提供。包内已经包含：

- `ProjectFourier.exe`
- `DftPrecompute.exe`
- `opencv_world4120.dll`
- 常见 MSVC 运行库 DLL
- `potrace.exe`
- 网页 viewer 和示例结果
- 包内中文说明 `README.md`

预编译包推荐给普通使用者；源码编译流程推荐给开发者。

### 从源码配置

Windows 下先双击：

```cmd
setup_environment.cmd
```

配置完成后双击：

```cmd
start_dft_viewer.cmd
```

浏览器会打开本地网页。如果没有自动打开，看 PowerShell 窗口里打印的地址，通常是：

```text
http://127.0.0.1:5174/dft_viewer.html
```

如果 `5174` 被占用，窗口会打印新的端口，例如 `5175`，按实际地址打开。

## 环境配置

`setup_environment.cmd` 会检查并配置：

- `Node.js`
- `MSBuild / Visual Studio C++ Build Tools`
- `OpenCV`
- `potrace`

脚本会生成本机专用的 `local_environment.ps1`，并尝试编译：

- `x64\Release\ProjectFourier.exe`
- `x64\Release\DftPrecompute.exe`

`local_environment.ps1` 是每台机器自己的路径配置，不需要提交，也不需要发给别人。

### 依赖

1. 安装 `Node.js LTS`
2. 安装 `Visual Studio` 或 `Build Tools`，需要包含 `Desktop development with C++`
3. 安装或解压 `OpenCV 4.12.0`
4. 安装或解压 `potrace`

### OpenCV

脚本会自动尝试查找 `OPENCV_DIR`。如果找不到，可以手动指定：

```powershell
powershell -ExecutionPolicy Bypass -File .\setup_environment.ps1 -OpenCVDir "C:\path\to\opencv\build"
```

`OpenCVDir` 应该指向 `opencv\build` 这一层，并且目录里需要有：

```text
include\opencv2\opencv.hpp
x64\vc16\lib\opencv_world4120.lib
x64\vc16\bin\opencv_world4120.dll
```

### potrace

`potrace` 下载下来通常是压缩包。脚本检测的是解压后的 `potrace.exe`，所以需要先把压缩包解压。

解压后目录结构类似：

```text
C:\tools\potrace\
  potrace.exe
```

然后运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\setup_environment.ps1 -PotraceDir "C:\tools\potrace"
```

如果 `potrace.exe` 已经在系统 `PATH` 里，可以不传 `-PotraceDir`。

### 常用配置命令

只检查环境、不重新编译：

```powershell
powershell -ExecutionPolicy Bypass -File .\setup_environment.ps1 -SkipBuild
```

手动指定 `OpenCV` 和 `potrace`：

```powershell
powershell -ExecutionPolicy Bypass -File .\setup_environment.ps1 -OpenCVDir "C:\path\to\opencv\build" -PotraceDir "C:\path\to\potrace"
```

## 网页端用法

### 推荐工作流程

从一张新图片生成动画的完整流程：

1. 启动 `start_dft_viewer.cmd`。
2. 在浏览器打开本地 viewer。
3. 在 `Processing -> Import Image` 选择 PNG、JPG、JPEG 或 BMP 图片。
4. 点击 `Upload`，图片会保存到 `uploads\`。
5. 点击 `Extract`，程序会提取线稿、SVG 和 component。
6. 提取完成后，在 `Image` 下拉框选择新图片名。
7. 在 `DFT Mode` 选择预运算模式，通常先用 `Both`。
8. 点击 `Run DFT Precompute`。
9. 预运算完成后，切到 `View -> Scene`。
10. 在 `Scene Mode` 选择 `Sequential`、`Simultaneous` 或 `Full SVG`。
11. 调整播放速度、显示轨迹、旋臂、固定中心等选项。
12. 如需改变动画质量或速度，在 `Scene DFT` 调整参数后重新运行 `Run DFT Precompute`。

网页中的 `Extract` 默认调用：

```cmd
x64\Release\ProjectFourier.exe --no-gui --no-clear <image>
```

网页中的 `Run DFT Precompute` 会调用：

```cmd
x64\Release\DftPrecompute.exe
```

网页左侧主要分为：

- `Source`：选择当前图片、通道、查看模式。
- `Processing`：导入图片、提取线稿、运行 DFT 预运算。
- `Scene DFT`：调整预运算参数。
- `Playback`：控制播放、录制、显示原图/轨迹/旋臂。
- `DFT`：只在 `Component` 模式下显示，用于单个 component 的实时 DFT 调试。

### 从新图片开始

1. 在 `Processing -> Import Image` 选择图片。
2. 点击 `Upload` 上传图片。
3. 点击 `Extract` 提取线稿和 component SVG。
4. 提取完成后，`Image` 下拉框会出现新图片名。
5. 选择 `DFT Mode`，通常先用 `Both`。
6. 点击 `Run DFT Precompute`。
7. 预运算完成后，切到 `View -> Scene` 查看动画。

`Extract` 会调用：

```cmd
x64\Release\ProjectFourier.exe --no-gui --no-clear <image>
```

`Run DFT Precompute` 会调用：

```cmd
x64\Release\DftPrecompute.exe
```

### Scene 模式

`View -> Scene` 用来查看已经预运算好的动画数据。

常用选项：

- `Scene Mode -> Sequential`：按 component 顺序逐个绘制。
- `Scene Mode -> Simultaneous`：多个 component 同时绘制。
- `Scene Mode -> Full SVG`：把整张线稿按 component 顺序拼成一个整体后做 DFT。
- `Channel -> XDoG_Guide / XDoG_Support`：切换线稿通道。
- `Speed`：播放速度。
- `Progress`：手动拖动画进度。
- `Full channel`：显示完整线稿背景。
- `Original path`：显示原始采样路径。
- `Rotating arms`：显示旋臂。
- `Trace`：显示绘制轨迹。
- `Fixed center`：旋臂中心固定在画面中心。
- `Record`：录制当前动画。

### Scene DFT 参数

这些参数会在点击 `Run DFT Precompute` 时生效，改完参数后需要重新预运算。

- `Params For`：选择正在编辑哪种模式的参数。
- `Samples`：每个 loop 的基础采样数。
- `Arms`：傅里叶旋臂数量。
- `Min Loop`：每个 loop 至少保留的采样点数。
- `Min Length`：过滤掉过短 loop。
- `Limit Loop`：是否启用 loop 长度上限。
- `Max Length`：启用 `Limit Loop` 后，超过这个长度的 loop 会被切成更短的 loop。
- `Smooth`：预运算前对采样路径做平滑的次数。
- `Duration`：动画时长。
- `FPS`：Scene 播放目标帧率。
- `Draw Stride`：轨迹绘制时隔几个点连线一次；数值越大越轻，但更粗糙。
- `Time Power`：Sequential 中按 component 长度分配播放时间的权重。
- `Arm Parts`：Simultaneous 中显示旋臂的 component 数量。
- `Full SVG Order`：Full SVG 下 component 拼接顺序，通常用 `Nearest`。
- `Coeff Order`：Full SVG 下系数排序，通常用 `Frequency`。

如果感觉局部变形，可以打开 `Limit Loop` 并把 `Max Length` 调小后重新预运算；如果感觉 loop 太碎或播放变卡，可以把 `Max Length` 调大。

### Component 模式

`View -> Component` 用来调试单个 component，很多计算是在浏览器里实时做的。

常用选项：

- `Component`：选择 component 编号。
- `Samples`：实时采样点数。
- `Order`：旋臂排序方式。
- `Arms`：实时 DFT 的旋臂数量。
- `Full SVG direct DFT`：对整张 SVG 直接做实时 DFT。
- `Samples x components`：在 `Full SVG direct DFT` 下，把采样数按 component 数量放大。
- `Auto component`：自动切到下一个 component。
- `Clear previous component trace`：切换 component 时清除上一段轨迹。

注意：`Component` 模式里的实时 DFT 适合调试，数据量大时会卡。正式动画建议使用 `Scene` 模式，先做预运算再播放。

## 命令行用法

### 提取线稿

```cmd
x64\Release\ProjectFourier.exe --no-gui --no-clear <image>
```

输出目录：

```text
results_v2\<image_name>
```

主要输出：

- `comp\`：所有连通部分的 component SVG
- `XDoG_Guide.bmp/svg`
- `XDoG_Support.bmp/svg`

常用参数：

- `--intermediate`：保留过程中间图。
- `--clear`：清除已有输出。
- `--no-gui`：不显示 OpenCV 窗口。
- `--no-clear`：不清除已有输出。

提取程序完整参数：

| 参数 | 说明 |
| --- | --- |
| `--no-gui` | 不弹出 OpenCV 预览窗口，适合网页调用和命令行批处理。 |
| `--headless` | `--no-gui` 的等价别名。 |
| `--clear` | 运行前清空整个输出根目录，默认是 `results_v2`。慎用，会删除已有结果。 |
| `--no-clear` | 不清空已有结果，只覆盖当前图片对应的输出。网页默认使用这个参数。 |
| `--intermediate` | 保存中间处理图，方便排查线稿提取效果。 |
| `--save-intermediate` | `--intermediate` 的等价别名。 |
| `--output <目录>` | 指定输出根目录，默认是 `results_v2`。 |
| `--max-dim <数值>` | 限制处理时图片最长边，默认约 `1180`，最低会被限制到 `320`。 |
| `--internal-scale <数值>` | 内部处理放大倍率，默认约 `1.30`，最低为 `1.0`。 |
| `--no-lowres-enhance` | 关闭低分辨率图片增强。 |
| `--lowres-min-dim <数值>` | 低分辨率增强触发阈值，默认约 `640`。 |
| `--lowres-target-min-dim <数值>` | 低分辨率增强目标短边，默认约 `720`。 |
| `--lowres-max-scale <数值>` | 低分辨率增强最大放大倍率，默认约 `1.75`。 |

保留中间图：

```cmd
x64\Release\ProjectFourier.exe --no-gui --no-clear --intermediate <image>
```

中间图会输出到：

```text
results_v2\<image_name>\intermediate\
results_v2\<image_name>\art_intermediate_case.png
```

批量处理多个图片：

```cmd
x64\Release\ProjectFourier.exe --no-gui --no-clear uploads\a.png uploads\b.jpg uploads\c.bmp
```

### DFT 预运算

```cmd
x64\Release\DftPrecompute.exe --mode both <image_name>
```

常用模式：

- `--mode sequential`
- `--mode simultaneous`
- `--mode full_svg`
- `--mode both`
- `--mode all`

只处理单个通道：

```cmd
x64\Release\DftPrecompute.exe --mode sequential --channels XDoG_Guide <image_name>
```

使用指定配置文件：

```cmd
x64\Release\DftPrecompute.exe --config dft_scene_params.txt --mode all <image_name>
```

预运算参数：

| 参数 | 说明 |
| --- | --- |
| `--help` 或 `-h` | 显示命令行帮助。 |
| `--output <目录>` | 指定结果根目录，默认是 `results_v2`。 |
| `--config <文件>` | 指定 DFT 参数配置文件，默认是 `dft_scene_params.txt`。 |
| `--channels <列表>` | 指定通道，默认是 `XDoG_Guide,XDoG_Support`。 |
| `--mode <模式>` | 指定预运算模式。 |
| `--modes <列表>` | `--mode` 的等价写法，支持逗号分隔。 |

输出：

```text
results_v2\<image>\dft_data\<channel>_<mode>.json
results_v2\<image>\FourierCoefficient\
```

## 动画导出

单 SVG 动画导出支持固定中心和整张线稿直接 DFT。

固定中心旋臂：

```cmd
node export_dft_animation.mjs --image=art --channel=XDoG_Guide --fixed-center=true --samples=4096 --arms=512 --duration=12 --hold=3
```

整张线稿直接 DFT：

```cmd
node export_dft_animation.mjs --image=art3 --channel=XDoG_Guide --source=full --dft-mode=single --fixed-center=true --samples=128 --full-samples-per-component=true --max-full-samples=16384 --arms=1024 --duration=12 --hold=3
```

整场 Scene HTML 导出：

```cmd
node export_dft_scene_animation.mjs --image=art --channel=XDoG_Guide --mode=sequential --fixed-center=true
```

批处理等价写法：

```cmd
export_dft_animations.cmd art XDoG_Guide 0 4096 512 12 3 full single true
```

## 配置文件

预运算和部分网页默认参数在：

```text
dft_scene_params.txt
```

常用参数：

- `duration`：动画默认时长。
- `hold`：动画播完后的停留时间。
- `target_fps`：动画帧率。
- `draw_stride`：轨迹绘制步长。
- `record_seconds`：录制时长。
- `component_time_power`：曲线长度对播放时间的权重。
- `sequential.samples`
- `sequential.arms`
- `sequential.max_loop_length`
- `simultaneous.samples`
- `simultaneous.arms`
- `full_svg.samples`
- `full_svg.arms`
- `full_svg.order`

网页端 `Scene DFT` 面板里的参数会在运行预运算时覆盖配置文件中的对应值。

## 注意事项

- `potrace` 压缩包必须先解压，脚本检测的是 `potrace.exe`。
- `Component` 模式是浏览器实时 DFT，大采样量会卡；正式动画建议用 `Scene` 预运算。
- `Full SVG direct DFT` 和 `Samples x components` 在 component 数量很多时计算量很大。
- 如果网页看不到新结果，刷新页面并确认预运算任务已经完成。
- 如果改了服务端脚本，重新运行 `start_dft_viewer.cmd`，旧的 Node 服务不会自动加载新代码。
