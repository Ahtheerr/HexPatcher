# HexPatcher

HexPatcher is a Windows ASI memory patcher that applies patches to the loaded executable from a matching INI file. It must be loaded as an ASI through Ultimate ASI Loader or another compatible ASI loader.

## Build

Open a Visual Studio Developer PowerShell and build the solution:

```powershell
msbuild HexPatcher.slnx /p:Configuration=Release /p:Platform=x64
```

The Release DLL is written to:

```text
x64/Release/HexPatcher.dll
```

Rename the built DLL to the desired `.asi` filename before using it with an ASI loader.

## Requirements

HexPatcher must be loaded as an ASI. Install [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader), or another compatible ASI loader, for the target application. Place the renamed `.asi` file in the folder expected by that loader.

## INI File Naming

The INI file must use the same base name as the loaded ASI and must be placed in the same folder.

```text
MyPatch.asi -> MyPatch.ini
PersonaPatch.asi -> PersonaPatch.ini
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

`Offsets` accepts one or more comma-separated offsets. By default offsets are RVAs relative to the target executable base address. Set `Raw = true` when using file offsets from tools such as HxD or 010 Editor.

String data always receives a trailing `00` byte unless it already ends with `00`. Direct hex patches write exactly the bytes listed, so `Data = 90 90` writes only `90 90`.

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

When `Pointer = true`, HexPatcher allocates new memory in the current process, writes `Data` there, then writes the allocated virtual address at each offset.

This writes an absolute in-memory pointer, not an RVA, relative pointer, or file offset. The pointer size follows the ASI build architecture: 4 bytes for x86 builds and 8 bytes for x64 builds. Use this only when the target executable expects a normal absolute pointer at the patched offset.

Pointer patch data is null-terminated automatically unless it already ends with `00`.

```ini
[MoveString]
Offsets = 0xE7B918
Pointer = true
Type = string
Data = This text lives in newly allocated memory.
```

With `Pointer = false` or omitted, HexPatcher writes the patch data bytes directly at each offset instead.

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
