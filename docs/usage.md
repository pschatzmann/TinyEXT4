# Usage

## Preparing a card

TinyEXT4 does not format. Create the file system on Linux:

```bash
sudo mkfs.ext4 -L data /dev/sdX1     # a partition (MBR or GPT)
sudo mkfs.ext4 /dev/sdX              # or the whole card
truncate -s 64M disk.img && mkfs.ext4 disk.img   # or an image file
```

The default `mkfs.ext4` settings work as they are: journal, 64bit, flex_bg and metadata_csum. A smaller block size (`-b 1024`) uses less space for small files. Disabling the journal (`-O ^has_journal`) saves space, but it is optional: TinyEXT4 leaves the journal alone.

## Mounting

```c++
Ext4FS fs;
fs.begin(driver);               // read/write, whole device or first ext partition
fs.begin(driver, true);         // read only
fs.begin(driver, false, 1);     // second ext partition
...
fs.end();                       // write everything, mark as cleanly unmounted
```

`begin()` mounts read-only if the device is write protected, or if the file system has features that TinyEXT4 can not write (see [features](features.md)). It also mounts read-only if the file system was not unmounted cleanly or still needs journal recovery: run `e2fsck` on Linux first. Check this with `isReadOnly()`.

When the first change is made, TinyEXT4 marks the file system as *not clean*. `end()` marks it as clean again. If the board loses power before `end()`, Linux reports this at the next mount, and `e2fsck` repairs the file system. There is no journal, so changes that were not written yet are lost.

## Files

```c++
File f = fs.open("/data/log.txt", "a");
f.println(value);          // File is an Arduino Stream on Arduino
f.write(buffer, len);
f.close();                 // writes the inode and syncs the device
```

| Mode | Meaning |
|---|---|
| `"r"` | read (default) |
| `"r+"` | read and write, no truncation |
| `"w"` / `"w+"` | create or truncate, write (and read) |
| `"a"` / `"a+"` | create, append each write at the end (and read) |

You can also pass `OpenFlags` (`Read | Write | Create | Truncate | Append`).

Other File methods: `read(buf, len)`, `read()`, `peek()`, `available()`, `seek(pos)`, `position()`, `size()`, `truncate(size)`, `flush()`, `name()`, `isDirectory()`, `getLastWrite()`, `mode()`, `inodeNumber()`.

`seek()` past the end of a writable file and then a write creates a sparse file, and the hole reads as zeros. The size and block list of a file live in the `File` object until `flush()` or `close()`; the destructor also flushes. Don't write to one file through two `File` objects at the same time.

## Directories

```c++
fs.mkdir("/data");
File dir = fs.open("/data");
DirEntry e;
while (dir.readDir(e)) {                 // "." and ".." are skipped
  Serial.println(e.name);                // e.inode, e.type, e.isDirectory()
}
dir.rewindDirectory();
for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
  Serial.println(f.name());
}
fs.rmdir("/data");                       // must be empty
```

## Other operations

```c++
fs.exists("/a.txt");
fs.remove("/a.txt");
fs.rename("/a.txt", "/dir/b.txt");       // moves files and directories; replaces a target file
FileInfo info; fs.stat("/a.txt", info);  // size, mode, uid, gid, times, links
char target[256]; fs.readlink("/link", target, sizeof(target));
fs.sync();                               // write all cached changes
fs.totalBytes(); fs.freeBytes(); fs.usedBytes(); fs.freeInodes();
fs.volumeName();
```

New files and directories get the owner and permission bits set with `setOwner(uid, gid)` (default 0/0) and `setPermissions(0644, 0755)`. Timestamps come from `setTimeCallback()`. On the desktop the default is `time()`; on Arduino the default is 0, so pass a function that returns the Unix time, e.g. from NTP or an RTC:

```c++
fs.setTimeCallback([]() -> uint32_t { return time(nullptr); });
```

## Errors

Methods return `false`, an empty `File` or a short count when they fail. `lastError()` returns the `Error` code and `lastErrorText()` a description:

```c++
if (!fs.mkdir("/data")) Serial.println(fs.lastErrorText());   // e.g. "exists"
```

Define `TINYEXT4_LOG 1` before the include to print diagnostics.
