import 'package:flutter_test/flutter_test.dart';
import 'package:fsr_mat_tester/main.dart';

void main() {
  testWidgets('shows the FSR tester shell', (tester) async {
    await tester.pumpWidget(const FsrTesterApp());

    expect(find.text('FSR 毯检测工具'), findsOneWidget);
  });
}
