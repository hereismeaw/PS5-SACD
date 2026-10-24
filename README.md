<div align="center">

# PS5-SACD

### DSD format tools and a real DSD → PCM conversion core

แยกไฟล์ DSD ได้ แปลง DSD เป็น PCM ด้วย FIR filter จริง และตรวจ build สำหรับ PS5 SDK

**C++23 · DSF · DSDIFF (uncompressed DSD) · streaming FIR decimator · PS5 toolchain**

</div>

---

## English

PS5-SACD is an early-stage C++ project for SACD/DSD format research. It provides
host-side file parsers, a streaming mono DSD-to-PCM conversion core, and a small
PS5 payload that reports DSF metadata.

### What works

| Component | Current support |
| --- | --- |
| DSF parser | Validates the container and exposes logical per-channel DSD bytes, preserving the file bit order. |
| DSDIFF parser | Reads uncompressed `DSD ` data, sample rate, channel layout, and interleaved channel bytes. |
| DSD → PCM | Stateful mono converter with a windowed-sinc Blackman FIR low-pass, rational output scheduling, and FIR tail flushing. |
| PS5 build | Builds the DSF info payload and cross-compiles the DSDIFF parser and FIR conversion source with the verified SDK compiler. |

### Not implemented

`DST` decoding, SACD ISO image parsing/extraction, multichannel orchestration,
PS5 audio output, and a playable or signed PS5 title are not implemented. DSDIFF
files using DST or another compression type are rejected as unsupported; they
are never treated as decoded DSD.

### Build and test on Windows

Install CMake 3.25+, Ninja, and a C++23 host compiler.

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

The test suite covers DSF parsing, uncompressed DSDIFF parsing and channel
layout, explicit DST rejection, DSD bit order, FIR filtering, and streaming
results across different input chunk sizes.

### Build the PS5 payload and core sources

The Windows helper expects Git Bash, MinGW `g++`/`make`, LLVM 18–23, and an
installed `ps5-payload-sdk`. The installed Prospero Clang 23.1.4 compiler and
its standard library were checked for C++23 `<expected>` support before the
PS5 build was enabled.

```powershell
.\tools\ps5\build-windows.ps1 -SdkPath 'C:\path\to\ps5-payload-sdk'
```

The build produces `out/build/ps5/dsf_info.elf` and compiles the DSDIFF parser
and DSD-to-PCM source as PS5-target object files. The payload itself reports
DSF metadata; it does not play audio. The ELF is intended for a compatible
payload loader, not the PS5 home screen.

### Library use

`DsfFile::read_channel_bytes` and `DsdiffFile::read_channel_bytes` expose one
channel's DSD bytes in the bit order reported by the format. Create one
`DsdToPcmConverter` per channel, pass the DSD and PCM sample rates plus the bit
order, then call `process()` for each byte block and `finish()` at end of
stream. The parsed file is a non-owning view: keep its source bytes alive while
reading from it.

The DSDIFF parser follows the public [DSDIFF 1.5 specification](https://www.sonicstudio.com/pdf/dsd/DSDIFF_1.5_Spec.pdf).

---

## ภาษาไทย

PS5-SACD เป็นโปรเจกต์ C++ ระยะแรกสำหรับศึกษาไฟล์ SACD/DSD มี parser สำหรับ
ไฟล์บนเครื่อง host, แกนแปลง DSD เป็น PCM แบบ streaming และ payload ขนาดเล็ก
สำหรับอ่าน metadata ของ DSF บน PS5

### ส่วนที่ทำงานได้แล้ว

| ส่วน | ความสามารถปัจจุบัน |
| --- | --- |
| DSF parser | ตรวจโครงสร้างไฟล์และอ่านข้อมูล DSD แยกตาม channel โดยรักษาลำดับบิตของไฟล์ |
| DSDIFF parser | อ่านข้อมูล `DSD ` ที่ไม่บีบอัด, sample rate, channel layout และข้อมูล channel แบบ interleaved |
| DSD → PCM | ตัวแปลง mono แบบมี state ใช้ windowed-sinc Blackman FIR low-pass, จัดตำแหน่ง output ตามอัตราสุ่ม และ flush FIR เมื่อจบข้อมูล |
| PS5 build | สร้าง payload อ่าน DSF metadata และ cross-compile source ของ DSDIFF กับ FIR converter ด้วย compiler ของ SDK ที่ตรวจแล้ว |

### ส่วนที่ยังไม่รองรับ

ยังไม่มี DST decoder, การอ่านหรือ extract จาก SACD ISO, ตัวจัดการหลาย channel,
การส่งเสียงออกทาง PS5 หรือเกม PS5 ที่เล่นได้และผ่านการเซ็นรับรอง ไฟล์ DSDIFF
ที่ใช้ DST หรือ compression แบบอื่นจะถูกแจ้งว่าไม่รองรับ และจะไม่ถูกนับเป็น
ข้อมูล DSD ที่ decode แล้ว

### Build และทดสอบบน Windows

ติดตั้ง CMake 3.25 ขึ้นไป, Ninja และ C++23 compiler สำหรับ host

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

ชุดทดสอบครอบคลุม DSF, DSDIFF แบบไม่บีบอัดและลำดับ channel, การปฏิเสธ DST,
ลำดับบิต DSD, FIR filtering และผลลัพธ์ streaming เมื่อแบ่งข้อมูลเป็น block ต่างขนาดกัน

### Build payload และ source สำหรับ PS5

สคริปต์ Windows ต้องใช้ Git Bash, MinGW `g++`/`make`, LLVM 18–23 และ
`ps5-payload-sdk` ที่ติดตั้งแล้ว ได้ตรวจ Prospero Clang 23.1.4 และ standard
library จริงว่ารองรับ C++23 `<expected>` ก่อนเปิดใช้ C++23 กับ PS5 build

```powershell
.\tools\ps5\build-windows.ps1 -SdkPath 'C:\path\to\ps5-payload-sdk'
```

ผล build คือ `out/build/ps5/dsf_info.elf` พร้อม object files สำหรับ DSDIFF
parser และ DSD-to-PCM source ตัว payload อ่าน DSF metadata เท่านั้น
ยังไม่ได้เล่นเสียง และ ELF นี้ใช้กับ payload loader ที่เข้ากันได้ ไม่ใช่แอป
ที่เปิดจากหน้า Home ของ PS5

### วิธีใช้ library

`DsfFile::read_channel_bytes` และ `DsdiffFile::read_channel_bytes` ใช้อ่าน DSD
ของ channel เดียวตาม bit order ของฟอร์แมต สร้าง `DsdToPcmConverter` แยกหนึ่งตัว
ต่อหนึ่ง channel โดยระบุ DSD/PCM sample rate และ bit order จากนั้นส่ง byte block
ให้ `process()` ต่อเนื่อง และเรียก `finish()` เมื่ออ่านจบ parser คืนค่าเป็น
view ที่ไม่ถือครองหน่วยความจำ จึงต้องเก็บ buffer ต้นทางไว้จนกว่าจะอ่านเสร็จ

parser DSDIFF อ้างอิง [ข้อกำหนด DSDIFF 1.5](https://www.sonicstudio.com/pdf/dsd/DSDIFF_1.5_Spec.pdf)
