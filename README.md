# HexPatcher

HexPatcher is a Windows DLL/ASI memory patcher that applies patches to the loaded executable from a matching INI file. It is intended for runtime patching of strings, raw bytes, and file-backed data.

## Build

Open a Visual Studio Developer PowerShell and build the solution:

```powershell
msbuild HexPatcher.slnx /p:Configuration=Release /p:Platform=x64
```

The Release DLL is written to:

```text
x64/Release/HexPatcher.dll
```

If using it as an ASI, rename the built DLL to the desired `.asi` filename.

## INI File Naming

The INI file must use the same base name as the loaded DLL or ASI and must be placed in the same folder.

```text
MyPatch.asi -> MyPatch.ini
HexPatcher.dll -> HexPatcher.ini
```

The DLL writes a simple log beside itself:

```text
MyPatch.asi -> MyPatch.log
```

## Basic Patch Format

Every non-special INI section is treated as a patch entry. Section names do not need to be unique; duplicate names are allowed and each entry is applied.

```ini
[PatchName]
Offsets = 0x123456, 0x123460
Raw = true
Type = hex
Data = 90 90 90
```

`Offsets` accepts one or more comma-separated offsets. By default offsets are RVAs. Set `Raw = true` when using file offsets from tools such as HxD or 010 Editor.

Data always receives a trailing `00` byte unless it already ends with `00`.

## Patch Types

String patch:

```ini
[ReplaceText]
Offsets = 0xE7A0D0
Pointer = true
Type = string
Data = Hello world!
```

Hex patch:

```ini
[NopInstruction]
Offsets = 0x14D7DD
Raw = true
Type = hex
Data = 90 90
```

File patch:

```ini
[InjectFileData]
Offsets = 0x200000
Type = file
Data = data\payload.bin
```

## Pointer Patches

When `Pointer = true`, HexPatcher allocates new memory, writes `Data` there, then writes the allocated address at each offset.

```ini
[MoveString]
Offsets = 0xE7B918
Pointer = true
Type = string
Data = This text lives in newly allocated memory.
```

## Version-Specific Offsets

`[Different_Versions]` can detect executable versions by reading bytes at one offset. The detected key enables matching `Offsets_<Key>` entries.

```ini
[Different_Versions]
Offsets = 0x80
Raw = true
MS = 01 E9 EC
ST = 63 6B 41

[VersionedPatch]
Offsets_MS = 0x14D7DD
Offsets_ST = 0x147C2D
Raw = true
Type = hex
Data = 61
```

Plain `Offsets` still applies regardless of detected version.

## Custom Font Encoding

`[Font]` changes how `Type = string` is encoded. Unmapped characters fall back to their byte value.

```ini
[Font]
á=9C DB
ç=9C E1
\n=0A

[TextPatch]
Offsets = 0xE7A0E8
Pointer = true
Type = string
Data = Olá\nmundo!
```

## Log Output

The log reports the patch count, load time, and failed entries:

```text
Patches applied: 123
Load time: 4.25 ms
Failed patches: 1
- PatchName: no matching Offsets
```
