# Magic mount

> This project is created for fun! :>

OverlayFS style based on Magisk's Magic mount logic (basically bind mount) that allows you to combine multiple directories into one similar to overlayfs. Support whiteout and trusted opaque behavior of overlayfs.

In short, this is OverlayFS re-implemented as magic mount logic. However, it doesn't support `upperdir` behavior found on OverlayFS for writable because it is actually just tmpfs + bind mounts.

Usage:

```bash
./magic-mount [-v] [-r] [-n NAME] /mnt/dir1 /mnt/dir2 /mnt/dir3 /mnt/merged
```

## Whiteout file/folder to mark delete

The whiteout file is character node with 0:0 (major, minor)

> When a whiteout is found in the upper level of a merged directory, any matching name in the lower level is ignored, and the whiteout itself is also hidden.

```bash
mkdir -p /data/adb/app
chcon u:object_r:system_file:s0 /data/adb/app
mknod /data/adb/app/Stk c 0 0
./magic-mount -r /data/adb/app /system/app /system/app

```

## Mark as replace

Folder with `trusted.overlay.opaque:y` attribute will be considered as trusted opaque

> Where the upper filesystem contains an opaque directory, any directory in the lower filesystem with the same name is ignored.

```bash
mkdir -p /data/adb/app/Stk
chcon -R u:object_r:system_file:s0 /data/adb/app
setfattr -n trusted.overlay.opaque -v y /data/adb/app/Stk
./magic-mount -r /data/adb/app /system/app /system/app

```

## Important

Note: Magic-mount is read-only. Extremely ineffective than overlayfs, don't use magic-mount with directory that includes large numbers of file/directory. It is recommended to use magic mount on folder that you actually need.
