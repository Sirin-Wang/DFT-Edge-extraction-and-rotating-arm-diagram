# ProjectFourier 一键环境配置

双击：

```cmd
setup_environment.cmd
```

脚本会做这些事：

- 检查 `Node.js`、`MSBuild / Visual Studio C++ Build Tools`、`OpenCV`、`potrace`
- 自动发现本机的 `OPENCV_DIR` 和 `potrace.exe`
- 生成本机专用的 `local_environment.ps1`
- 编译 `ProjectFourier.exe` 和 `DftPrecompute.exe`
- 把 OpenCV 运行时 DLL 复制到 `x64\Release`

配置完成后双击：

```cmd
start_dft_viewer.cmd
```

## 依赖

- Node.js LTS
- `Visual Studio 2022/2026` 或 `Build Tools`，安装 `Desktop development with C++`
- `OpenCV 4.12.0`，要求目录里存在：
  - `include\opencv2\opencv.hpp`
  - `x64\vc16\lib\opencv_world4120.lib`
  - `x64\vc16\bin\opencv_world4120.dll`
- potrace，要求能找到 `potrace.exe`

## 手动指定路径

如果脚本找不到 OpenCV 或 potrace，可以这样运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\setup_environment.ps1 -OpenCVDir "C:\path\to\opencv\build" -PotraceDir "C:\path\to\potrace"
```

只检查环境、不重新编译：

```powershell
powershell -ExecutionPolicy Bypass -File .\setup_environment.ps1 -SkipBuild
```

`local_environment.ps1` 是每台机器自己的路径配置，不需要提交或转发。
