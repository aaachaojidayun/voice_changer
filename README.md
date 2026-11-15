---
AIGC:
    Label: "1"
    ContentProducer: 001191440300708461136T1XGW3
    ProduceID: 1b5cf64a8d8cc5070fb6ed397770b513_e15f1c65e0d111f19919525400638852
    ReservedCode1: U+SfKSoIw83HsrXXKOadYpLOKTxLf4NTrgBIOi0itkeiy137SbP6Dd8PE4+SWXaJerbhT0gAx2Kz6453oxlu+9T0+AlRAm8/kbxTTeGWUB761z1IRGs+Y0r9u5qL9nJpoonlieTWUuAQROHso79t9m+8WfAAQ2dAopYsXf+m/ibJ55oQBy8/xclFrMA=
    ContentPropagator: 001191440300708461136T1XGW3
    PropagateID: 1b5cf64a8d8cc5070fb6ed397770b513_e15f1c65e0d111f19919525400638852
    ReservedCode2: U+SfKSoIw83HsrXXKOadYpLOKTxLf4NTrgBIOi0itkeiy137SbP6Dd8PE4+SWXaJerbhT0gAx2Kz6453oxlu+9T0+AlRAm8/kbxTTeGWUB761z1IRGs+Y0r9u5qL9nJpoonlieTWUuAQROHso79t9m+8WfAAQ2dAopYsXf+m/ibJ55oQBy8/xclFrMA=
---

# voice_changer —— 纯 C++17 离线变声器（变调，时长不变）

零第三方依赖、零 SIMD 手写指令、可交叉编译到 aarch64 Linux 的参考实现。
输入 WAV，输出变调后的 WAV，时长与输入严格一致（帧数逐帧相等）。

## 算法

变调而不变时长，必须把"时间伸缩"和"频率搬移"两件事分开做：

```
输入 ──► PhaseVocoder 时间伸缩（拉长 α 倍） ──► Catmull-Rom 重采样（步长 α） ──► 输出
                时长 × α                          时长 × (1/α)，频率 × α
```

两者复合：时长 × α × (1/α) = 1，频率 × α。这就是"变调不变速"。

### 为什么不能直接重采样

直接重采样 α 倍会连同时间一起改变（α=1.5 变成 1.5 倍速的花栗鼠音）。
必须先用 Phase Vocoder 把信号拉长 α 倍（音高保持不变），再重采样压缩回来。

### 为什么不用 PSOLA

PSOLA 依赖基音周期检测，在噪声、气声、复音、清辅音上会明显劣化，且需要额外
的基频跟踪器。Phase Vocoder 对任意信号都成立，且实现可论证、可测试。

### Phase Vocoder 的三个关键点

1. **相位累积**

   相邻帧的分析相位不能直接拿来当合成相位用——因为合成 hop 和分析 hop 不同，
   直接取会让相位在帧间跳变，听感是"颗粒感 / 金属声"。
   正确做法是维护一个累积的合成相位 `ψ_m`，每帧自己往前推进。

2. **频率 bin 修正**

   FFT 的第 k 个 bin 标称角频率是 `ω_k = 2πk/N`，但信号在该 bin 里的真实频率
   通常不是 `ω_k`。真实频率要从帧间相位差反推：

   ```
   dφ = princarg( φ_m - φ_{m-1} - ω_k · H_a )      // 减去标称推进量，折回主值
   ω_true = ω_k + dφ / H_a                          // 真实瞬时角频率
   ψ_m = princarg( ψ_{m-1} + ω_true · H_s )         // 按真实频率推进合成相位
   ```

   没有这一步，变调后的频率会"整格跳变"（误差最大可达一个 bin 宽度）。

3. **OLA 归一化**

   逐帧加窗后叠加，再除以窗平方和 `Σ w²[n]`。不做归一化会因为以 hop 为周期的
   增益起伏产生爆音。本实现边累加边记录 `w²`，同时把已完成段清零复用。

### 关键参数

| 项 | 取值 | 说明 |
| --- | --- | --- |
| FFT 窗长 N | 1024（默认）/ 2048 | 44.1k/48k 下 1024 约 21ms，兼顾时间分辨率与频率分辨率 |
| 分析 hop | N/4 | 周期 Hann 窗下严格 COLA |
| 合成 hop | round(α · N/4)，上限 N/2 | 保证 OLA 至少两次重叠 |
| 窗函数 | 周期 Hann（分母 N） | 非对称式 (N-1) 才能保证 hop=N/4 时 COLA |
| 采样格式 | float32 交织 | 内部全程 float，落盘转 16-bit PCM |
| 内存 | 构造期预分配 | `process()` 内零 malloc |

## 目录结构

```
voice_changer/
├── CMakeLists.txt
├── cmake/aarch64-linux-gnu.cmake     交叉编译工具链
├── include/vc/
│   ├── ring_buffer.hpp               定长环形缓冲（运行期零分配）
│   ├── fft.hpp                       实数 radix-2 FFT，接口对齐 kiss_fftr
│   ├── resampler.hpp                 Catmull-Rom 三次插值重采样
│   ├── phase_vocoder.hpp             STFT + 相位累积 + OLA
│   ├── pitch_shifter.hpp             变调对外唯一入口
│   └── wav_io.hpp                    WAV 读写
├── src/                              对应实现 + main.cpp
└── tests/test_main.cpp               自写断言测试，零依赖
```

## 构建

### CMake

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### 直接 g++（无 cmake 环境）

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude \
    src/fft.cpp src/resampler.cpp src/phase_vocoder.cpp \
    src/pitch_shifter.cpp src/wav_io.cpp src/main.cpp \
    -o build/voice_changer
```

## 用法

```bash
voice_changer <input.wav> <output.wav> [--pitch 0.5~2.0] [--window 1024|2048]
```

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--pitch` | 1.0 | 变调倍数。1.5 = 升调 1.5 倍，0.8 = 降调 |
| `--window` | 1024 | FFT 窗长，2 的幂 |

示例：

```bash
./voice_changer in.wav out.wav --pitch 1.5
./voice_changer in.wav out.wav --pitch 0.8 --window 2048
```

## 接口约定（嵌入到自己的工程时）

```cpp
vc::PitchShifter ps(sampleRate, pitch, fftSize);
ps.process(in, out, frames);   // 严格等长，有固定延迟 latency()
ps.flush();                    // 排空管线
ps.readTail(buf, cap);         // 取残余
// 离线恢复对齐：从输出第 latency() 个样本起取 L 个
```

固定延迟 `latency()` = N + 8 个样本（N=1024 时 1032）。离线场景下裁掉即可，
实时场景可容忍或在前端补偿。

## 实时变声（Windows）

`src/live_win.cpp` 提供 `vc_live`：直接拿麦克风说话、从耳机听变声结果，
不需要先录音再转文件。核心算法完全复用 `vc::PitchShifter`。

### 构建

```powershell
g++ -std=c++17 -O2 -Iinclude src/fft.cpp src/resampler.cpp src/phase_vocoder.cpp `
    src/pitch_shifter.cpp src/wav_io.cpp src/live_win.cpp -o build/vc_live.exe -lole32 -luuid
```

CMake 构建时只在 `WIN32` 下生成 `vc_live` 目标，离线 CLI 与核心库保持零依赖、
仍可交叉编译到 aarch64。

### 使用

```powershell
.\build\vc_live.exe --list                       # 先看设备编号
.\build\vc_live.exe --capture 0 --render 0 --pitch 1.5
```

| 参数 | 说明 |
| --- | --- |
| `--list` | 列出活动状态的麦克风与播放设备编号（标注系统默认项） |
| `--capture N` | 用编号 N 的麦克风，省略则用系统默认麦克风 |
| `--render N` | 用编号 N 的播放设备，省略则用系统默认播放设备 |
| `--pitch` | 初始变调倍数，默认 1.5，允许 0.05~8.0 |
| `--fft` | 512 / 1024 / 2048 / 4096，默认 1024 |

运行中在控制台输入数字回车即时切换音调，`s` 打印运行状态，`q` 退出。
**耳机常常不是默认播放设备**（Realtek 声卡会把默认保持在"扬声器"），
用 `--list` 确认后用 `--render N` 指定，否则声音会从外放出来。

### 实现要点

- **WASAPI 共享模式 + 事件驱动**。两端都请求事件回调，避免轮询空转；
  混音格式由系统给出（本机为 48 kHz / 2 声道 / float32），代码支持
  float32 与 16/32-bit PCM，采样率不一致时直接报错而不是悄悄变速。
- **线程模型**：采集线程内完成「取包 → 混单声道 → `PitchShifter::process`
  → 写入无锁环形缓冲」；播放线程只做「从环形缓冲取 → 写设备缓冲」。
  跨线程只用 `vc::SpscRing`（原子读写指针 + acquire/release），全程无锁、
  音频路径零 malloc。
- **启动顺序**：先启采集、等它真正产出第一块音频后再启播放。麦克风从
  `Start` 到出第一个包常有数百毫秒，同时启动会连续欠载、开头一段空白。
- **运行期换音调不爆音**：不直接改 `Resampler::setStep()`（那会打断相位累积，
  听感是"啪"的爆音），而是在非实时线程构造新的 `PitchShifter`，
  处理线程在下一块开始用 50 ms 交叉淡化切入新实例，旧实例延迟释放。

### 实测（本机 Realtek 声卡，48 kHz）

10 秒连续运行，每 10 ms 一块：

```
块数=940  欠载=0  溢出=0  积压=960 样本（稳定）
```

全链路延迟约 40~50 ms（采集周期 10 ms + 算法固定延迟 21.5 ms + 播放缓冲）。

### 已知限制

- 仅 Windows（WASAPI）。Linux 端可换 ALSA/PipeWire，`SpscRing` 与算法层不用改。
- 输出为单声道处理结果复制到左右声道，不做立体声分别处理。
- 算法固定延迟 `latency()` 未在实时路径上补偿（实时场景表现为约 21 ms 延迟）。

## 已验证结果

在本机 WSL Ubuntu 24.04 / g++ 13.3.0 上编译并跑通，`-Wall -Wextra -Wpedantic`
零警告。

单元测试 19 项全通过：

| 用例 | 输入 | 期望 | 实测 |
| --- | --- | --- | --- |
| 1 | 48k 单声道 1kHz，pitch 1.5 | 1500 Hz | 1500.00 Hz |
| 2 | 48k 单声道 1kHz，pitch 0.5 | 500 Hz | 500.15 Hz |
| 3 | 44.1k 单声道 1kHz，pitch 2.0，N=2048 | 2000 Hz | 1999.97 Hz |
| 4 | 48k 立体声 1kHz，pitch 1.5 | 1500 Hz | 1500.00 Hz |
| 5 | WAV 读写往返 | 误差 ≤ 1 LSB | 3.04e-05 |

端到端 CLI 验证（3 秒 48k 立体声 440Hz）：

```
输入 144000 帧 → 输出 144000 帧（时长精确保持）
440Hz × 1.5 → 实测 660 Hz
440Hz × 0.8 → 实测 352 Hz
输出文件字节数与输入完全一致（576044 字节）
```

## 交叉编译到 Zynq / aarch64

```bash
sudo apt install g++-aarch64-linux-gnu
cmake -B build-aarch64 -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake
cmake --build build-aarch64 -j
```

全部代码为标准 C++17，不含架构相关优化（无 `-march=native`、无 `-mavx`、
无手写 NEON），迁移零适配。

## 关于第三方库

原计划用 dr_wav（WAV IO）+ kissfft（FFT），但当前环境无法访问 GitHub，
因此内置了两份等价的最小实现：

- `include/vc/fft.hpp`：接口刻意对齐 `kiss_fftr`（实数输入、只暴露 N/2+1 bin）
- `include/vc/wav_io.hpp`：用法与 dr_wav 同构（整段读、整段写）

联网后若想换成原库，只需重写这两个文件的实现，调用方无需改动。

## 已知限制

- 仅支持 WAV；输出固定 16-bit PCM（输入支持 8/16/24/32-bit PCM 与 32-bit float）
- 仅支持单声道 / 立体声
- `--pitch` 限定 0.5~2.0；更极端的倍率会出现相位声码器固有的瞬态模糊
- 极端瞬态（鼓点、爆破音）有轻微 pre-echo，这是 Phase Vocoder 的固有特性
*（内容由AI生成，仅供参考）*
