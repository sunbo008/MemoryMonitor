# MemoryMoniter

简单的 Windows GUI 程序，用于申请和监视进程内存峰值。

构建方式：

- 使用 CMake（推荐）:
  - 需要安装 CMake 并将其加入 PATH。
  - 推荐通过命令行或批处理脚本执行构建（见下文）。

- 直接使用 MSVC (cl.exe):
  - 在 "Developer Command Prompt for VS" 中运行: 直接运行 `build_vs2019.bat`。

常见问题:
- 如果缺少 `cmake`，请安装 CMake 或使用 Visual Studio 的 Developer Prompt。
- 如果缺少 `cl.exe`，打开 Visual Studio 的 Developer Command Prompt 或执行 Visual Studio 安装目录下的 `vcvarsall.bat`。

本程序说明
----------------
- 功能：在主窗口上半部分绘制进程内存使用曲线（蓝色：实时工作集，红色：历史峰值），下半部分列出已申请的内存块并允许双击释放。
- 数据采集：程序通过周期性（默认 1 秒）查询本进程的内存信息，保存到历史数组并绘制曲线。

关键函数说明：GetProcessMemoryInfo
-----------------------------------
程序使用 Windows API 函数 `GetProcessMemoryInfo`（声明于 `<psapi.h>`）来查询当前进程的内存使用信息。核心点：

- 作用：`GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))` 会填充 `PROCESS_MEMORY_COUNTERS` 结构体，其中 `WorkingSetSize` 表示当前工作集（RSS），`PeakWorkingSetSize` 表示历史峰值工作集。程序把这些值作为“当前内存”和“峰值”绘入图表。
- 为什么重要：这是从操作系统层面准确获取进程物理内存使用（工作集）的标准方法，适用于 Windows 平台。相比自行统计堆分配，它反映的是整个进程在物理内存中的占用。
- 注意事项：
  - 该函数需要包含 `<psapi.h>` 并且在链接时通常不需要额外库（我们在 CMake 中明确链接了 `Psapi`）。
  - `WorkingSetSize` 单位为字节，程序对其进行了单位格式化（B/KB/MB/GB）。
  - 获取的数据是系统视角的瞬时值，采样间隔越小波动越明显；程序使用历史缓冲并进行缩放以便可视化。

如何运行（快速一键）
--------------------
- 使用批处理一键构建（推荐，默认 VS2019 x64 Debug）：
  ```powershell
  cd D:\workspace\MemoryMoniter
  .\build_vs2019.bat
  ```

更多
----
- 可以在 `main.cpp` 中调整采样频率（SetTimer 的间隔）、历史长度或图形样式。
- 如果要监视其它进程，可修改数据采集部分以使用 `OpenProcess` + `GetProcessMemoryInfo` 对特定 PID 进行查询（注意权限）。
