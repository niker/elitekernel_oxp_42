#!/system/bin/sh

# EliteKernel: deploy modules and misc files
mount -o remount,rw /system
chmod -R 0644 system/lib/modules
cp -fR /modules/*  /system/lib/modules
chmod -R 0644 system/lib/modules
find /system/lib/modules -type f -name '*.ko' -exec chown 0:0 {} \;

# make sure init.d is ok
chmod -R 775 /system/etc/init.d
find /system/etc/init.d -type f -name '*' -exec chown 0:2000 {} \;
#chgrp -R 2000 /system/etc/init.d
mount -o remount,ro /system
sync

# force insert modules that are required
insmod /system/lib/modules/bcmdhd.ko
insmod /system/lib/modules/baseband_xmm_power2.ko
insmod /system/lib/modules/raw_ip_net.ko
insmod /system/lib/modules/baseband_usb_chr.ko
insmod /system/lib/modules/cdc_acm.ko
touch /data/local/em_modules_deployed


# feed urandom data to /dev/random to avoid system blocking (potential security risk, use at own peril!)
/elitekernel/rngd --rng-device=/dev/urandom --random-device=/dev/random --background --feed-interval=60

# startup time tweaks:

# enable smartdimmer
echo "1" > /sys/devices/tegradc.0/smartdimmer/enable

#I/O tweaks
mount -o async,remount,noatime,nodiratime,delalloc,noauto_da_alloc,barrier=0,nobh /cache /cache
mount -o async,remount,noatime,nodiratime,delalloc,noauto_da_alloc,barrier=0,nobh /data /data
mount -o async,remount,noatime,nodiratime,delalloc,noauto_da_alloc,barrier=0,nobh /sd-ext /sd-ext
mount -o async,remount,noatime,nodiratime,delalloc,noauto_da_alloc,barrier=0,nobh /devlog /devlog
echo "3072" > /sys/block/mmcblk0/bdi/read_ahead_kb;
echo "3072" > /sys/block/mmcblk0/queue/read_ahead_kb;

# activate delayed config to override ROM
/system/xbin/busybox nohup /system/bin/sh /elitekernel/elitekernel_delayed.sh 2>&1 >/dev/null &


