# 智能垫出厂检测项目 (FSR Blanket Production Tester)

本项目用于智能垫 (FSR Blanket) 出厂时的传感器质量检测，通过软硬件结合的方式，直观地显示垫子上7个 FSR（薄膜压力传感器）通道的工作状态。

## 📁 项目结构 (Workspace Structure)

本项目包含两个主要子项目：

- **`firmware/`**: 下位机固件代码
  - 基于 PlatformIO 和 Arduino 框架编写。
  - 主控芯片：**ESP32-WROOM**。
  - 负责采集传感器的 ADC 数据，并通过蓝牙低功耗 (BLE) 将数据发送出去。
- **`app/`**: 上位机测试应用
  - 基于 **Flutter** 框架开发的 Android 应用程序。
  - 用于接收 BLE 广播/通知，并在界面上实时展示 7 个 FSR 传感器的受力位置。

## ⚙️ 工作原理 (How it Works)

1. **数据采集**：ESP32 持续读取 7 个 FSR 通道的 ADC 值并进行求平均滤波处理。
2. **数据传输**：测试仪通过 BLE 自动向外发送通知 (BLE notifications)。
3. **状态判定**：当某个通道的平均 ADC 值**低于 `3000`** 时，App 端会将该通道的显示状态高亮，表明该位置的传感器触发正常（无损坏）。

## 📲 下载与安装 (Download)

你可以直接下载预编译好的安装包进行测试：

1. 前往仓库右侧的 [Releases](https://github.com/18877592402/SmartMatTest_Project/releases) 页面。
2. 找到最新发布版本，在 **Assets** 栏目下点击下载 `app-release.apk`。
3. 在安卓手机上安装并打开（需开启 BLE 蓝牙权限）。

## 🛠️ 开发与编译环境 (Development Setup)

### 1. 固件端 (Firmware)
- 推荐 IDE: VS Code + [PlatformIO 插件](https://platformio.org/)
- 编译与烧录：使用 PlatformIO 打开 `firmware` 文件夹，连接 ESP32 开发板后点击 Upload 即可。

### 2. 移动端 (App)
- 依赖环境: [Flutter SDK](https://flutter.dev/) 和 Android Studio
- 编译运行：在终端进入 `app` 目录，执行 `flutter run` 将应用安装到安卓测试机上。
