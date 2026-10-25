<div align="center">

# PS5-SACD

### DSD tools, real DSD → PCM conversion, and lossless SACD track extraction

<p>
  <img src="https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white" alt="C++23" />
  <img src="https://img.shields.io/badge/Formats-DSF%20%7C%20DSDIFF%20%7C%20SACD-243447" alt="DSF, DSDIFF, SACD" />
  <img src="https://img.shields.io/badge/Audio-1--bit%20DSD%20%7C%20PCM-7c3aed" alt="DSD and PCM audio" />
</p>

**From SACD sectors to clean DSDIFF — and from DSD to PCM, with the bytes accounted for.**

อ่าน metadata และตำแหน่ง track, extract plain DSD จาก SACD image เป็น DSDIFF, แปลง DSD เป็น PCM ด้วย FIR filter จริง และ build core สำหรับ PS5 SDK

**C++23 · DSF · DSDIFF · SACD TOC + extraction · streaming FIR · PS5 toolchain**

</div>

---

## English

PS5-SACD is a C++ project for SACD/DSD format research. It provides host-side
DSF, DSDIFF, and SACD image parsers, a lossless extractor for plain DSD SACD
tracks, a streaming DSD-to-PCM conversion core, and a small PS5 payload that
reports DSF metadata.

### What works

| Component | Current support |
| --- | --- |
| DSF parser | Validates the container and exposes logical per-channel DSD bytes, preserving the file bit order. |
| DSDIFF parser | Reads uncompressed `DSD ` data, sample rate, channel layout, and interleaved channel bytes. |
| DSD → PCM | Stateful mono converter with a windowed-sinc Blackman FIR low-pass, rational output scheduling, and FIR tail flushing. |
| `dsd2wav` | Converts DSF or uncompressed DSDIFF files to 24-bit PCM WAVE_FORMAT_EXTENSIBLE WAV. It maps documented DSF layouts and recognized DSDIFF stereo/LSCO layouts. |
| `sacdinfo` | Reports logical-sector count, audio area properties, TOC selection, and verified track extents. |
| `sacdextract` | Reassembles and extracts plain DSD64 tracks to DSDIFF for standard 2, 5, and 6 channel layouts. Audio bytes are copied losslessly. |
| SACD image parser | Reads 2048-byte logical-sector master/area TOCs (including backup fallback), area attributes, and bounded `SACDTRL1` track extents. |
| SACD-Text | `sacdinfo` decodes album/disc labels and common track credits from ISO-646 and ISO-8859-1 into UTF-8. |
| PS5 build | Builds the DSF info payload and a PS5 Browser launcher for Music Room, and cross-compiles DSDIFF, FIR, SACD TOC, text, and uncompressed SACD sector parsing sources with the SDK compiler. |
| PS5 Music Room | A controller-friendly, full-screen browser player for PCM WAV files served read-only from a computer on the same LAN. |

### Not implemented

`DST` decoding, SACD-Text character sets ISO-8859-1-with-escape and the legacy
Japanese/Korean/Chinese encodings, 2064-byte raw-sector dumps, direct audio
output from the native payload, and a signed PS5 home-screen title are not
implemented. The Music Room plays host-converted PCM WAV through the PS5
Browser; it does not decode DSD on the console. ISO-646 and plain ISO-8859-1
labels are decoded;
other character sets are reported as unsupported. `sacdextract` accepts raw
2048-byte logical-sector images and extracts DSD64 frame formats 2/3 with
standard 2/5/6 channel counts; other sample rates and channel counts are
rejected. DSDIFF files using DST or another compression type are rejected as
unsupported. A playable or signed PS5 title is outside the current payload build.

### Build and test on Windows

Install CMake 3.25+, Ninja, and a C++23 host compiler.

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

The test suite covers DSF and DSDIFF parsing, SACD master/area TOCs and backup
selection, SACD-Text offsets and ISO-8859-1 conversion, safe track extents,
audio-sector packet reassembly, DST rejection, 2/5/6 channel DSDIFF extraction,
DSD bit order, FIR filtering, and streaming across different input chunk sizes.
End-to-end fixtures exercise `sacdinfo` → `sacdextract` → `dsd2wav` and validate
WAV output for DSF and DSDIFF, including a partial DSF byte.

### Convert a file to PCM WAV

```powershell
cmake --build --preset windows-debug
.\out\build\windows-debug\dsd2wav.exe .\album.dsf .\album.wav
.\out\build\windows-debug\dsd2wav.exe --rate 48000 .\album.dff .\album-48k.wav
```

The default PCM rate is 44.1 kHz for the 44.1 kHz DSD family and 48 kHz for the
48 kHz family. Output uses 24-bit integer PCM. Stereo output is tagged as
front-left/front-right, including stereo DSDIFF files whose CHNL identifiers
give the reverse order. Documented DSF layouts and recognized DSDIFF 5.0/5.1
layouts with `LSCO` are mapped to WAVE speakers. Other layouts retain source
channel order with an unspecified speaker mask. The standard RIFF/WAVE 4 GiB
size limit applies. The command refuses to overwrite an existing output file
and publishes the result only after conversion finishes.

### Inspect and extract a SACD track

```powershell
cmake --build --preset windows-debug
.\out\build\windows-debug\sacdinfo.exe .\album.iso
.\out\build\windows-debug\sacdextract.exe .\album.iso 1 .\track-01.dff
.\out\build\windows-debug\sacdextract.exe --area multichannel .\album.iso 1 .\track-01-5.1.dff
```

Extraction defaults to the stereo area. Use `--area multichannel` for the
multichannel area. The extractor supports uncompressed DSD64 in standard 2, 5,
and 6 channel SACD order; the five and six channel mappings follow the published
[SACD Audio Signal Properties](https://www.sonicstudio.com/pdf/dsd/SACD_SignalPropertiesV13.pdf).
It rejects DST, other DSD rates, and nonstandard channel counts. The output is
uncompressed DSDIFF, not PCM.

### Build the PS5 payload and core sources

The Windows helper expects Git Bash, MinGW `g++`/`make`, LLVM 18–23, and an
installed `ps5-payload-sdk`. The installed Prospero Clang 23.1.4 compiler and
its standard library were checked for C++23 `<expected>` support before the
PS5 build was enabled.

```powershell
.\tools\ps5\build-windows.ps1 `
  -SdkPath 'C:\path\to\ps5-payload-sdk' `
  -MusicUiUrl 'http://192.168.0.29:8765/'
```

Replace the example URL with the address printed by `serve.py`.

The build produces `out/build/ps5/dsf_info.elf` and
`out/build/ps5/music_ui.elf`, and compiles the DSDIFF parser, DSD-to-PCM source,
SACD image/text parsers, and uncompressed audio-sector parser as PS5-target
object files. `dsf_info.elf` reports DSF metadata. `music_ui.elf` opens the PS5
Browser at the Music Room URL; both ELF files are for a compatible payload
loader, not the PS5 home screen.

### Open the PS5 Music Room

Put PCM WAV files in a folder on the computer, then start the read-only library
server. Keep the computer and PS5 on the same LAN, and use the computer's LAN IP
address in the build command:

```powershell
python .\tools\ui\serve.py --library 'D:\Music'
```

The server prints a URL such as `http://192.168.0.29:8765/`. Build the launcher
with that URL, then send `music_ui.elf` to a compatible payload loader while the
server is running:

```powershell
.\tools\ps5\build-windows.ps1 `
  -SdkPath 'C:\path\to\ps5-payload-sdk' `
  -MusicUiUrl 'http://192.168.0.29:8765/'
```

The browser player supports play, pause, seeking, volume, search, queue, repeat,
shuffle, keyboard input, and common controller buttons. Convert DSF/DSDIFF or
extract a supported SACD track on the computer first, then put the resulting WAV
in the shared folder. The library server only lists and streams WAV files; it
does not modify them.

### Library use

`DsfFile::read_channel_bytes` and `DsdiffFile::read_channel_bytes` expose one
channel's DSD bytes in the bit order reported by the format. Create one
`DsdToPcmConverter` per channel, pass the DSD and PCM sample rates plus the bit
order, then call `process()` for each byte block and `finish()` at end of
stream. The parsed file is a non-owning view: keep its source bytes alive while
reading from it.

`extract_track_to_dsdiff` accepts an image byte span, selected audio area, track
number, and seekable output stream. It parses only the selected track's sectors
and returns an error for DST or unsupported formats instead of copying them as
plain DSD.

`parse_text` reads one language slot and returns UTF-8 metadata for ISO-646 or
ISO-8859-1. It reports unsupported legacy encodings and malformed offsets as
errors, so callers can keep showing the rest of the SACD report.

The DSDIFF parser follows the public [DSDIFF 1.5 specification](https://www.sonicstudio.com/pdf/dsd/DSDIFF_1.5_Spec.pdf).
The SACD TOC and track-list layout follows the public ScarletBook structures
used by [SACD Ripper](https://github.com/hank/sacd-ripper/blob/master/libs/libsacd/scarletbook.h).

---

## ภาษาไทย

PS5-SACD เป็นโปรเจกต์ C++ สำหรับศึกษาไฟล์ SACD/DSD มี parser สำหรับ DSF,
DSDIFF และ SACD image, ตัว extract plain DSD จาก SACD track, แกนแปลง DSD เป็น
PCM แบบ streaming และ payload ขนาดเล็กสำหรับอ่าน metadata ของ DSF บน PS5

### ส่วนที่ทำงานได้แล้ว

| ส่วน | ความสามารถปัจจุบัน |
| --- | --- |
| DSF parser | ตรวจโครงสร้างไฟล์และอ่านข้อมูล DSD แยกตาม channel โดยรักษาลำดับบิตของไฟล์ |
| DSDIFF parser | อ่านข้อมูล `DSD ` ที่ไม่บีบอัด, sample rate, channel layout และข้อมูล channel แบบ interleaved |
| DSD → PCM | ตัวแปลง mono แบบมี state ใช้ windowed-sinc Blackman FIR low-pass, จัดตำแหน่ง output ตามอัตราสุ่ม และ flush FIR เมื่อจบข้อมูล |
| `dsd2wav` | แปลงไฟล์ DSF หรือ DSDIFF แบบไม่บีบอัดเป็น WAV 24-bit PCM แบบ WAVE_FORMAT_EXTENSIBLE พร้อม map layout มาตรฐานของ DSF และ DSDIFF stereo/LSCO |
| `sacdinfo` | แสดงจำนวน logical sector, คุณสมบัติ audio area, TOC ที่เลือก และขอบเขต sector ของ track |
| `sacdextract` | รวม packet และ extract plain DSD64 เป็น DSDIFF สำหรับ layout มาตรฐาน 2, 5 และ 6 channel โดยไม่เปลี่ยน audio bytes |
| SACD image parser | อ่าน master/area TOC แบบ logical sector 2048-byte พร้อม backup fallback, metadata และ track extent จาก `SACDTRL1` ที่ตรวจขอบเขตแล้ว |
| SACD-Text | `sacdinfo` ถอดรหัสชื่อ album/disc และเครดิต track จาก ISO-646 กับ ISO-8859-1 เป็น UTF-8 |
| PS5 build | สร้าง payload อ่าน DSF metadata, launcher เปิด Music Room ใน PS5 Browser และ cross-compile source ของ DSDIFF, FIR, SACD TOC, text และ audio-sector parser ด้วย compiler ของ SDK |
| PS5 Music Room | music player แบบเต็มจอและควบคุมด้วย controller สำหรับ PCM WAV ที่แชร์แบบอ่านอย่างเดียวจากคอมพิวเตอร์ใน LAN เดียวกัน |

### ส่วนที่ยังไม่รองรับ

ยังไม่มี DST decoder, การถอด SACD-Text แบบ ISO-8859-1 ที่มี escape sequence
หรือ charset เก่าในกลุ่ม Japanese/Korean/Chinese, raw-sector dump ขนาด
2064-byte และการส่งเสียงโดยตรงจาก native payload ยังไม่มีแอป PS5 ที่เปิดจากหน้า
Home หรือผ่านการรับรอง ส่วน Music Room เล่น PCM WAV ที่แปลงจากคอมพิวเตอร์ผ่าน
PS5 Browser; ยังไม่ได้ถอดรหัส DSD บนเครื่อง PS5 ปัจจุบันถอด ISO-646 และ ISO-8859-1 ปกติได้
และจะแจ้ง charset อื่นว่าไม่รองรับ `sacdextract` รับ image แบบ logical sector
2048-byte และ extract DSD64 frame format 2/3 สำหรับ layout มาตรฐาน 2/5/6 channel
เท่านั้น sample rate หรือจำนวน channel แบบอื่นจะถูกปฏิเสธ ไฟล์ DSDIFF ที่ใช้ DST
หรือ compression แบบอื่นจะถูกแจ้งว่าไม่รองรับ เกม PS5 ที่เล่นได้หรือผ่านการ
เซ็นรับรองยังอยู่นอกขอบเขตของ payload ปัจจุบัน

### Build และทดสอบบน Windows

ติดตั้ง CMake 3.25 ขึ้นไป, Ninja และ C++23 compiler สำหรับ host

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

ชุดทดสอบครอบคลุม DSF/DSDIFF, master/area TOC และ backup, SACD-Text offsets และ
การแปลง ISO-8859-1, track extent, packet reassembly, การปฏิเสธ DST, การ extract
DSDIFF layout 2/5/6 channel, FIR/streaming และ WAV output จาก DSF/DSDIFF
พร้อมทดสอบการทำงานต่อเนื่อง `sacdinfo` → `sacdextract` → `dsd2wav`

### แปลงไฟล์เป็น PCM WAV

```powershell
cmake --build --preset windows-debug
.\out\build\windows-debug\dsd2wav.exe .\album.dsf .\album.wav
.\out\build\windows-debug\dsd2wav.exe --rate 48000 .\album.dff .\album-48k.wav
```

ค่าเริ่มต้นคือ 44.1 kHz สำหรับ DSD ตระกูล 44.1 kHz และ 48 kHz สำหรับตระกูล
48 kHz ไฟล์ output เป็น integer PCM 24-bit เสียง stereo ระบุ speaker เป็น
front-left/front-right และจัด channel stereo ตาม CHNL ของ DSDIFF รวมถึง map
layout 5.0/5.1 เมื่อมี LSCO ส่วน layout ที่ไม่รู้จักรักษาลำดับ channel ต้นฉบับ
และระบุ speaker mask ว่าไม่กำหนด จำกัดขนาดไฟล์ตาม RIFF/WAVE ที่ 4 GiB และคำสั่งจะไม่
เขียนทับ output ที่มีอยู่ โดยจะเผยแพร่ไฟล์เมื่อแปลงเสร็จเท่านั้น

### ตรวจข้อมูลและ extract track จาก SACD

```powershell
cmake --build --preset windows-debug
.\out\build\windows-debug\sacdinfo.exe .\album.iso
.\out\build\windows-debug\sacdextract.exe .\album.iso 1 .\track-01.dff
.\out\build\windows-debug\sacdextract.exe --area multichannel .\album.iso 1 .\track-01-5.1.dff
```

ค่าเริ่มต้นใช้ stereo area; ระบุ `--area multichannel` เพื่อเลือก multichannel
area รองรับ plain DSD64 channel count 2, 5 และ 6 ตามลำดับมาตรฐานที่ระบุใน
[SACD Audio Signal Properties](https://www.sonicstudio.com/pdf/dsd/SACD_SignalPropertiesV13.pdf)
ผลลัพธ์เป็น DSDIFF แบบไม่บีบอัด ไม่ใช่ PCM และ DST/อัตรา DSD หรือ channel layout
อื่นจะถูกปฏิเสธอย่างชัดเจน

### Build payload และ source สำหรับ PS5

สคริปต์ Windows ต้องใช้ Git Bash, MinGW `g++`/`make`, LLVM 18–23 และ
`ps5-payload-sdk` ที่ติดตั้งแล้ว ได้ตรวจ Prospero Clang 23.1.4 และ standard
library จริงว่ารองรับ C++23 `<expected>` ก่อนเปิดใช้ C++23 กับ PS5 build

```powershell
.\tools\ps5\build-windows.ps1 `
  -SdkPath 'C:\path\to\ps5-payload-sdk' `
  -MusicUiUrl 'http://192.168.0.29:8765/'
```

เปลี่ยน URL ตัวอย่างให้ตรงกับ URL ที่ `serve.py` แสดง

ผล build คือ `out/build/ps5/dsf_info.elf` และ
`out/build/ps5/music_ui.elf` พร้อม object files สำหรับ DSDIFF parser,
DSD-to-PCM, SACD TOC/text และ parser ของ uncompressed audio sectors ไฟล์
`dsf_info.elf` อ่าน DSF metadata ส่วน `music_ui.elf` เปิด PS5 Browser ไปยัง
Music Room ทั้งสองไฟล์ใช้กับ payload loader ที่เข้ากันได้ ไม่ใช่แอปจากหน้า Home
ของ PS5

### เปิด PS5 Music Room

วางไฟล์ PCM WAV ในโฟลเดอร์บนคอมพิวเตอร์ แล้วเปิด library server แบบอ่านอย่างเดียว
ให้คอมพิวเตอร์และ PS5 อยู่ใน LAN เดียวกัน:

```powershell
python .\tools\ui\serve.py --library 'D:\Music'
```

server จะแสดง URL เช่น `http://192.168.0.29:8765/` นำ URL ของคอมพิวเตอร์ไปใส่
ตอน build launcher จากนั้นส่ง `music_ui.elf` เข้า payload loader ที่รองรับขณะ
server ยังทำงาน:

```powershell
.\tools\ps5\build-windows.ps1 `
  -SdkPath 'C:\path\to\ps5-payload-sdk' `
  -MusicUiUrl 'http://192.168.0.29:8765/'
```

player รองรับ play/pause, seek, volume, ค้นหา, queue, repeat, shuffle,
คีย์บอร์ด และปุ่ม controller ทั่วไป ต้องแปลง DSF/DSDIFF หรือ extract track ที่
รองรับบนคอมพิวเตอร์ก่อน แล้ววาง WAV ในโฟลเดอร์ที่แชร์ server จะแสดงและส่งเฉพาะ
ไฟล์ WAV โดยไม่แก้ไขไฟล์ต้นฉบับ

### วิธีใช้ library

`DsfFile::read_channel_bytes` และ `DsdiffFile::read_channel_bytes` ใช้อ่าน DSD
ของ channel เดียวตาม bit order ของฟอร์แมต สร้าง `DsdToPcmConverter` แยกหนึ่งตัว
ต่อหนึ่ง channel โดยระบุ DSD/PCM sample rate และ bit order จากนั้นส่ง byte block
ให้ `process()` ต่อเนื่อง และเรียก `finish()` เมื่ออ่านจบ parser คืนค่าเป็น
view ที่ไม่ถือครองหน่วยความจำ จึงต้องเก็บ buffer ต้นทางไว้จนกว่าจะอ่านเสร็จ

`extract_track_to_dsdiff` รับ byte span ของ image, audio area, หมายเลข track
และ output stream ที่ seek ได้ โดยอ่านเฉพาะ sector ของ track ที่เลือก และคืน
ข้อผิดพลาดเมื่อพบ DST หรือ format ที่ไม่รองรับ แทนการเขียนข้อมูลนั้นเป็น DSD ดิบ

`parse_text` อ่านภาษาใน slot ที่เลือกและคืน metadata UTF-8 สำหรับ ISO-646 หรือ
ISO-8859-1 หากพบ charset เก่าหรือ offset ที่ผิดรูปแบบจะคืน error เพื่อให้ caller
ยังแสดงข้อมูล SACD ส่วนอื่นต่อได้

`ps5_sacd::formats::sacd_iso::parse` รับ byte span ของ SACD image ที่ใช้ sector
2048-byte แล้วคืนค่า area attributes และ LSN/length ของ track แต่ละรายการ
parser ตรวจ master/area TOC pointers และทุก track extent ให้อยู่ภายใน image
ส่วน `AudioFrameAssembler` รวม packet ของ plain DSD frame และ extractor เขียน
track ที่รองรับลง DSDIFF

parser DSDIFF อ้างอิง [ข้อกำหนด DSDIFF 1.5](https://www.sonicstudio.com/pdf/dsd/DSDIFF_1.5_Spec.pdf)
โครงสร้าง SACD TOC และ track list อ้างอิง ScarletBook structures ที่เผยแพร่ใน
[SACD Ripper](https://github.com/hank/sacd-ripper/blob/master/libs/libsacd/scarletbook.h)
