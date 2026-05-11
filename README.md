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
Root: xxxx
Node: C:\Program Files\nodejs\node.exe

Starting local viewer server...
DFT viewer: http://127.0.0.1:5174/dft_viewer.html
Keep this window open while using the viewer.
```

用来监测端口

所以如果窗口里打印的是 5174，浏览器打开：

```
http://127.0.0.1:5174/dft_viewer.html
```

如果窗口提示 5174 被占用并改到 5175，就用它打印出来的实际地址。
