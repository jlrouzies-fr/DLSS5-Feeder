# GPU Compatibility Update

## อัปเดตความเข้ากันได้ของ GPU

ระบบสามารถตรวจจับ GPU ที่ติดตั้งอยู่จริงโดยอัตโนมัติ และเลือกโหมด Optimization ที่เหมาะสมกับฮาร์ดแวร์

หาก GPU ไม่รองรับ DLSS 5 ระบบจะเข้าสู่ Legacy/Fallback Mode โดยอัตโนมัติ

ฟีเจอร์ที่จำเป็นต้องใช้ DLSS 5 จะไม่ถูกเปิดใช้งานบน GPU ที่ไม่รองรับ

ระบบจะยึดความสามารถของฮาร์ดแวร์จริงเป็นหลัก

---

The application now automatically detects the installed GPU and determines the appropriate optimization mode.

GPUs that do not support DLSS 5 will be automatically placed into Legacy/Fallback mode.

DLSS 5-dependent features will not be enabled on unsupported hardware.

Detected hardware capability takes priority over manually selected settings.

---

## Compatibility Notice

If the detected GPU does not support DLSS 5, the system will remain in a safe fallback state and will not force unsupported DLSS 5 features on the user.

This is an evidence-based detection path that prefers real Windows hardware metadata over any hard-coded GPU assumptions.
