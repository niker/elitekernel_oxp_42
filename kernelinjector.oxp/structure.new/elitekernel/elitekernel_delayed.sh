#!/system/bin/sh

sleep 60 
# do the configuration again to override ROM and tegra hardcoded stuff

touch /data/local/em_delayed_tweaks

# start user init
# activate delayed config to override Kernel
/system/xbin/busybox nohup /system/bin/sh /data/local/userinit.sh 2>&1 >/dev/null &
/system/xbin/busybox nohup /system/bin/sh /data/local/zramswap.sh 2>&1 >/dev/null &


