import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

void main() => runApp(const FsrTesterApp());

class FsrTesterApp extends StatelessWidget {
  const FsrTesterApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'FSR 毯检测工具',
      theme: ThemeData(
        useMaterial3: true,
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.teal),
        scaffoldBackgroundColor: const Color(0xFFF0F2F5),
      ),
      home: const FsrHomePage(),
    );
  }
}

// --- 常量定义 ---
const String kServiceUuid = '2f3a0001-3c18-4b22-9c70-364d4f535200';
const String kNotifyUuid = '2f3a0002-3c18-4b22-9c70-364d4f535200';
const String kControlUuid = '2f3a0003-3c18-4b22-9c70-364d4f535200';

const List<String> kChannelLabels = [
  '头部', '右手', '左手', '右膝', '左膝', '右脚', '左脚'
];

// --- 数据模型 ---
class FsrData {
  final int mask;
  final List<int> values;
  final DateTime time;

  FsrData({required this.mask, required this.values, required this.time});

  bool isTriggered(int index) => (mask & (1 << index)) != 0;

  static FsrData? fromBytes(List<int> bytes) {
    if (bytes.length < 16 || bytes[0] != 0xA5) return null;
    final vals = <int>[];
    for (int i = 0; i < 7; i++) {
      vals.add(bytes[2 + i * 2] | (bytes[3 + i * 2] << 8));
    }
    return FsrData(mask: bytes[1], values: vals, time: DateTime.now());
  }
}

// --- 控制逻辑 (Controller) ---
class FsrController extends ChangeNotifier {
  BluetoothConnectionState connectionState = BluetoothConnectionState.disconnected;
  List<ScanResult> scanResults = [];
  bool isScanning = false;
  
  BluetoothDevice? connectedDevice;
  FsrData? lastData;
  String errorMessage = '';

  StreamSubscription? _scanSub;
  StreamSubscription? _stateSub;
  StreamSubscription? _dataSub;
  Timer? _uiLimitTimer;

  FsrController() {
    FlutterBluePlus.isScanning.listen((s) {
      isScanning = s;
      notifyListeners();
    });
  }

  Future<void> requestPermissions() async {
    await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.location,
    ].request();
  }

  void startScan() async {
    await requestPermissions();
    if (isScanning) return;
    
    scanResults.clear();
    errorMessage = '';
    
    _scanSub?.cancel();
    _scanSub = FlutterBluePlus.scanResults.listen((results) {
      // 过滤具有 FSR 服务或名称前缀的设备
      scanResults = results.where((r) {
        final name = r.advertisementData.advName.toUpperCase();
        return name.startsWith('FSR-MAT') || 
               r.advertisementData.serviceUuids.any((u) => u.toString().toLowerCase() == kServiceUuid);
      }).toList();
      
      // 限制扫描时的刷新频率，避免卡顿
      if (_uiLimitTimer == null || !_uiLimitTimer!.isActive) {
        notifyListeners();
        _uiLimitTimer = Timer(const Duration(milliseconds: 600), () {});
      }
    });

    try {
      await FlutterBluePlus.startScan(timeout: const Duration(seconds: 15));
    } catch (e) {
      errorMessage = '扫描启动失败: $e';
      notifyListeners();
    }
  }

  Future<void> connect(BluetoothDevice device) async {
    await FlutterBluePlus.stopScan();
    errorMessage = '';
    notifyListeners();

    try {
      _stateSub?.cancel();
      _stateSub = device.connectionState.listen((s) {
        connectionState = s;
        if (s == BluetoothConnectionState.disconnected) {
          _dataSub?.cancel();
          connectedDevice = null;
        }
        notifyListeners();
      });

      await device.connect(timeout: const Duration(seconds: 10));
      connectedDevice = device;

      final services = await device.discoverServices();
      BluetoothCharacteristic? notifyChar;
      
      for (var s in services) {
        if (s.uuid.toString().toLowerCase() == kServiceUuid) {
          for (var c in s.characteristics) {
            if (c.uuid.toString().toLowerCase() == kNotifyUuid) notifyChar = c;
          }
        }
      }

      if (notifyChar != null) {
        _dataSub = notifyChar.lastValueStream.listen((bytes) {
          final data = FsrData.fromBytes(bytes);
          if (data != null) {
            lastData = data;
            notifyListeners();
          }
        });
        await notifyChar.setNotifyValue(true);
      } else {
        throw '未找到数据通知特征';
      }
    } catch (e) {
      errorMessage = '连接失败: $e';
      device.disconnect();
      notifyListeners();
    }
  }

  void disconnect() {
    connectedDevice?.disconnect();
    _dataSub?.cancel();
    connectedDevice = null;
    notifyListeners();
  }

  @override
  void dispose() {
    _scanSub?.cancel();
    _stateSub?.cancel();
    _dataSub?.cancel();
    _uiLimitTimer?.cancel();
    super.dispose();
  }
}

// --- UI 界面 ---
class FsrHomePage extends StatefulWidget {
  const FsrHomePage({super.key});

  @override
  State<FsrHomePage> createState() => _FsrHomePageState();
}

class _FsrHomePageState extends State<FsrHomePage> {
  final FsrController _ctrl = FsrController();

  @override
  void dispose() {
    _ctrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: _ctrl,
      builder: (context, _) {
        final isConnected = _ctrl.connectionState == BluetoothConnectionState.connected;
        
        return Scaffold(
          appBar: AppBar(
            title: const Text('FSR 毯检测工具', style: TextStyle(fontWeight: FontWeight.bold)),
            centerTitle: true,
            backgroundColor: Theme.of(context).colorScheme.inversePrimary,
            actions: [
              if (isConnected)
                IconButton(icon: const Icon(Icons.link_off), onPressed: _ctrl.disconnect)
              else
                IconButton(icon: const Icon(Icons.refresh), onPressed: _ctrl.isScanning ? null : _ctrl.startScan),
            ],
          ),
          body: isConnected ? _buildMonitorView() : _buildScanView(),
        );
      },
    );
  }

  // 连接后的监控视图
  Widget _buildMonitorView() {
    return SingleChildScrollView(
      padding: const EdgeInsets.all(16),
      child: Column(
        children: [
          Container(
            padding: const EdgeInsets.all(12),
            decoration: BoxDecoration(color: Colors.white, borderRadius: BorderRadius.circular(8)),
            child: Row(
              children: [
                const Icon(Icons.bluetooth_connected, color: Colors.teal),
                const SizedBox(width: 8),
                Expanded(child: Text('已连接: ${_ctrl.connectedDevice?.platformName ?? "未知"}', style: const TextStyle(fontWeight: FontWeight.bold))),
              ],
            ),
          ),
          const SizedBox(height: 24),
          // 头部
          Row(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              SizedBox(width: 160, height: 100, child: _buildSensorCard(0)),
            ],
          ),
          const SizedBox(height: 16),
          // 手部 (左手, 右手)
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceEvenly,
            children: [
              Expanded(child: SizedBox(height: 100, child: _buildSensorCard(2))),
              const SizedBox(width: 16),
              Expanded(child: SizedBox(height: 100, child: _buildSensorCard(1))),
            ],
          ),
          const SizedBox(height: 16),
          // 膝盖 (左膝, 右膝)
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceEvenly,
            children: [
              Expanded(child: SizedBox(height: 100, child: _buildSensorCard(4))),
              const SizedBox(width: 16),
              Expanded(child: SizedBox(height: 100, child: _buildSensorCard(3))),
            ],
          ),
          const SizedBox(height: 16),
          // 脚部 (左脚, 右脚)
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceEvenly,
            children: [
              Expanded(child: SizedBox(height: 100, child: _buildSensorCard(6))),
              const SizedBox(width: 16),
              Expanded(child: SizedBox(height: 100, child: _buildSensorCard(5))),
            ],
          ),
        ],
      ),
    );
  }

  Widget _buildSensorCard(int i) {
    final isPress = _ctrl.lastData?.isTriggered(i) ?? false;
    final val = _ctrl.lastData?.values[i] ?? 0;
    
    return Card(
      color: isPress ? Colors.green[400] : Colors.white,
      elevation: 2,
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          Text(kChannelLabels[i], style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold, color: isPress ? Colors.white : Colors.black87)),
          const SizedBox(height: 4),
          Text(val.toString(), style: TextStyle(fontSize: 20, fontWeight: FontWeight.w900, fontFamily: 'monospace', color: isPress ? Colors.white : Colors.teal[700])),
        ],
      ),
    );
  }

  // 未连接时的扫描视图
  Widget _buildScanView() {
    if (_ctrl.errorMessage.isNotEmpty) {
      return Center(child: Text(_ctrl.errorMessage, style: const TextStyle(color: Colors.red)));
    }

    if (!_ctrl.isScanning && _ctrl.scanResults.isEmpty) {
      return Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            const Icon(Icons.bluetooth_searching, size: 64, color: Colors.grey),
            const SizedBox(height: 16),
            ElevatedButton.icon(onPressed: _ctrl.startScan, icon: const Icon(Icons.search), label: const Text('开始扫描')),
          ],
        ),
      );
    }

    return Column(
      children: [
        if (_ctrl.isScanning) const LinearProgressIndicator(),
        Expanded(
          child: ListView.builder(
            itemCount: _ctrl.scanResults.length,
            itemBuilder: (ctx, i) {
              final r = _ctrl.scanResults[i];
              return Card(
                margin: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
                child: ListTile(
                  leading: const CircleAvatar(child: Icon(Icons.bluetooth)),
                  title: Text(r.advertisementData.advName.isNotEmpty ? r.advertisementData.advName : '未知板子'),
                  subtitle: Text(r.device.remoteId.str),
                  trailing: Text('${r.rssi} dBm', style: const TextStyle(fontWeight: FontWeight.bold)),
                  onTap: () => _ctrl.connect(r.device),
                ),
              );
            },
          ),
        ),
      ],
    );
  }
}
