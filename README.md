## 依赖

需要安装`openCV`与`potrace`库

## 功能

这个程序主要是想实现以下功能

- 先从图片提取线稿组件
- 再把组件转换成傅里叶绘制数据

具体流程为

1. 根据读取图片的最大尺寸缩放增强图片，以削弱低清晰度的影响
2. 构建人物/主体引导区域，作为接下来处理的对象
3. 在引导区域中进行线稿提取
4. 在线稿候选和引导的综合判断下构建更合适的主体区域
5. 通过`XDoG`算法得到最终线稿输出
6. 线稿通过`potrace`转为矢量图，并提取出连通部分
7. 对每个连通部分进行二次划分并`DFT`
8. 最终作为动图输出

主要算法为`LAB`色域分离+`Canny`+`XGoD`边缘提取

## 使用

项目有两个`.exe`文件，分别起处理线稿+提取连通部分和计算傅里叶系数的作用

`ProjectFourier.exe`会将图片提取线稿并输出普通图片与拟合的矢量图，输出结果保存在`/results_v2/<image_name>`中，包括

- 文件夹`comp`，包含所有连通部分的矢量图
- `XGoD_Guide/Support.bmp/svg`分别为第一次引导结果也最终结果的线稿普通图片与矢量图

```cmd
`x64\Release\ProjectFourier.exe --no-gui <image>`
```

其他参数：

- `--intermediate`保留过程中间图
- `--clear`清除已有输出
- `--no gui`不在窗口展示输出图

`DftPrecompute.exe`会直接计算已经处理过的线稿的离散傅里叶变换，并输出动画数据

- `dft_data`动画的数据文件
- `FourierCoefficient`顾名思义，是每个连通部分的傅里叶系数，方便接其他功能

```cmd
`x64\Release\DftPrecompute.exe <image_name>`
```

要查看动画请运行：

```cmd
.\start_dft_viewer.cmd
```

它会打开一个`PowerShell`窗口：

```cmd
ProjectFourier DFT viewer
Root: C:\Users\27107\source\repos\ProjectFourier
Node: C:\Program Files\nodejs\node.exe

Starting local viewer server...
DFT viewer: http://127.0.0.1:5174/dft_viewer.html
Keep this window open while using the viewer.
```

用来监测端口**（不要关闭！）**

所以如果窗口里打印的是 5174，浏览器打开：

```
http://127.0.0.1:5174/dft_viewer.html
```

如果窗口提示 5174 被占用并改到 5175，就用它打印出来的实际地址。

网页中的各个选项功能为：

- `View`用来切换已预运算的数据绘图`Scene`和实时运算`Component`
- `Image`切换对象
- `Channel`切换哪个矢量图为蓝本
- `Scene Mode`切换演示效果：逐部分绘制`Sequential`和所有部分同时绘制`Simultaneous`
- `Speed``Progress`调整速度和进程
- `Full Channel`显示你选择的通道的矢量图
- `Original path`显示当前原始输入路径/组件轮廓
- `Rotating arms`显示旋臂
- `Trace`显示绘制的路径
- `Arm visibility`旋臂可见度

`Component`模式下新增：

- `Component`查看指定序号的连通部分

- `Samples`采样点个数
- `Order`旋臂排序规则
- `Arms`旋臂个数
- `Auto component`在`Sequential`模式下自动切换到下一个连通部分
- `Clear previous component trace`在`Sequential`模式下自动清除上一个连通部分的轨迹
- `Record`录制，时间可在配置文件`dft_scene_params`中更改

配置文件主要是决定预运算的参数和一部分网页参数

- `duration`动画默认时间
- `hold`动画播完暂停时间
- `target_fps`动画帧数
- `draw_stride`每隔几个点连一次线
- `record_seconds`录制时间
- `component_time_power`曲线长度对播放时间的权重

最上方的都是默认参数，预运算时调用下面的参数



### 慎用`Componet`+`Simultaneous`，会很卡

## 新增动画导出用法

单`SVG`动画导出现在支持两个额外功能：

- 固定中心旋臂：用 `--fixed-center=true`，旋臂链的起点固定在 `SVG` `viewBox` 中心。
- 整张线稿直接 `DFT`：用 `--source=full --dft-mode=single`。如果存在 `results_v2/<image>/comp/<channel>_*.svg`，会先按组件编号顺序拼成一个全局有序路径源，再按总长度统一采样成一条曲线；否则退回直接读取 `results_v2/<image>/<channel>.svg`。

示例：

```cmd
node export_dft_animation.mjs --image=art --channel=XDoG_Guide --source=full --dft-mode=single --fixed-center=true --samples=4096 --arms=512 --duration=12 --hold=3
```

批处理等价写法：

```cmd
export_dft_animations.cmd art XDoG_Guide 0 4096 512 12 3 full single true
```

整场 HTML 导出也支持固定中心：

```cmd
node export_dft_scene_animation.mjs --image=art --channel=XDoG_Guide --mode=sequential --fixed-center=true
```

查看器 `dft_viewer.html` 的 `Component` 模式新增了两个开关：

- `Full SVG direct DFT`：优先按 `comp` 编号顺序拼成全局有序路径源，再统一采样成一条曲线 `DFT`。
- `Samples x components`：只在 `Full SVG direct DFT` 下生效，把 `Samples` 当作每个组件的采样预算，并把总采样数限制到 16384，避免浏览器卡死。
- `Fixed center`：把旋臂原点固定到画面中心。

`Scene` 模式新增 `Full SVG`，需要先预运算：

```cmd
x64\Release\DftPrecompute.exe --mode full_svg art3
```

`full_svg` 预运算会输出：

```cmd
results_v2\<image>\dft_data\<channel>_full_svg.json
```

它按 `comp` 编号顺序拼成全局有序路径源，默认总采样数是 `full_svg.samples * component_count`，不做上限裁剪。默认配置在 `dft_scene_params.txt`：

```txt
full_svg.samples=128
full_svg.arms=1024
```

命令行中也可以开启按组件放大采样：

```cmd
node export_dft_animation.mjs --image=art3 --channel=XDoG_Guide --source=full --dft-mode=single --fixed-center=true --samples=128 --full-samples-per-component=true --max-full-samples=16384 --arms=1024 --duration=12 --hold=3
```

`componet`模式不要直接用 `4096 * component_count` 这种规模，普通浏览器实时`DFT`基本不可用。
