# Supported Features

## Reading

| Feature | Support |
|---|---|
| Block sizes 1 KB - 64 KB | yes |
| Extents (any depth), uninitialized extents (read as zeros) | yes |
| Block maps (ext2/ext3: direct, indirect, double, triple) | yes |
| 64bit, flex_bg, meta_bg, sparse_super, sparse_super2 | yes |
| htree directories | yes (read linearly) |
| Sparse files | yes (holes read as zeros) |
| Fast and slow symbolic links | yes, `readlink()`; paths are not followed |
| Inline data | first 60 bytes (data in `i_block`) |
| Large files (> 4 GB) | yes |
| Journal | ignored; a file system that needs recovery can be read, but recent changes may be missing |
| MBR and GPT partitions | yes |

Unknown incompatible features (compression, journal devices, dirdata) prevent mounting.

## Writing

Writing needs the `extent` and `filetype` features. `mkfs.ext4` sets both by default. The following are kept up to date:

- block and inode bitmaps, including groups that `mkfs` left uninitialized (`BLOCK_UNINIT`, `INODE_UNINIT`, lazy inode tables)
- free counts in the group descriptors and the superblock, used directories, `itable_unused`
- `metadata_csum` checksums: superblock, group descriptors, bitmaps, inodes, extent tree blocks and directory blocks; also the older `gdt_csum`
- extent trees: contiguous blocks are merged into one extent, and the tree grows as needed (new leaves, new levels)
- directories: freed space is reused, and new blocks are added when needed
- link counts, including `dir_nlink` for directories with many subdirectories
- extended attribute blocks of deleted files (reference count and checksum)

Operations: create, write, append, overwrite, truncate (shrink and grow), sparse files, `mkdir`, `rmdir`, `remove`, `rename` (also replacing a file, and moving directories, which updates `..`).

The file system is mounted read-only when it:
- has no `extent` or `filetype` feature (ext2, ext3)
- uses `bigalloc`, `quota`, `project`, `inline_data` writes, `encrypt`, `casefold`, `mmp`, `verity` or other unknown read-only-compatible features
- needs journal recovery, was not unmounted cleanly, or has errors recorded

## Limitations

- No journaling. Writes go directly to their final location: a power loss in the middle of an operation can leave the file system inconsistent, and `e2fsck` repairs it. The journal on the disk is left untouched, so Linux keeps using it.
- Adding an entry to an htree directory removes the index, so the directory becomes a linear one. Linux handles this without problems, and `e2fsck -D` rebuilds the index. Only single-level indexes are converted: up to about 500 directory blocks with 4 KB blocks (tens of thousands of entries), or about 120 blocks with 1 KB blocks. Adding to directories with a deeper index fails with `Unsupported`.
- Writing to files that use block maps (created on ext2/ext3), to uninitialized extents (`fallocate`), or into holes in the middle of a file's extent list fails with `Unsupported`. Deleting such files works.
- Only the primary superblock and group descriptors are updated, as the kernel does. `e2fsck` refreshes the backups.
- No symlink, hard link, device node or extended attribute creation, and no permission checks.
- No formatting: use `mkfs.ext4`.

## Verification

The [test suite](desktop.md) creates images with `mke2fs` in 10 configurations (1/2/4 KB blocks, with and without metadata_csum, 64bit, journal and flex_bg, 128-byte inodes, MBR, ext2, ext3). It changes them with TinyEXT4 and checks every image with `e2fsck -fn`. It also reads the files back independently with `debugfs`. A separate test covers GPT partitions.
