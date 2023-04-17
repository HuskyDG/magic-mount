# Magic mount

> This project is created for fun! :>

OverlayFS style based on Magisk's Magic mount logic (basically bind mount) that allows you to combine multiple directories into one similar to overlayfs. Support whiteout and trusted opaque behavior of overlayfs.

Usage:

```bash
./magic-mount [-v] [-r] [-n NAME] /mnt/dir1 /mnt/dir2 /mnt/dir3 /mnt/merged
```

## Whiteout file/folder to mark delete

```bash
mkdir -p /data/adb/app
chcon u:object_r:system_file:s0 /data/adb/app
mknod /data/adb/app/Stk c 0 0
./magic-mount -r /data/adb/app /system/app /system/app

```

## Mark as replace

```bash
mkdir -p /data/adb/app/Stk
chcon -r u:object_r:system_file:s0 /data/adb/app
setfattr -n trusted.overlay.opaque -v y /data/adb/app/Stk
./magic-mount -r /data/adb/app /system/app /system/app

```

## Important

Note: Magic-mount is read-only. Extremly ineffective than overlayfs, don't use magic-mount with directory that includes large numbers of file/directory. It is recommended to use magic mount on folder that you actually need.
