adb root 
adb remount
adb shell mkdir -p /data/data/org.onaips.vnc/lib/
adb push nativeMethods/libs/armeabi-v7a/libdvnc_flinger_sdk17.so /data/data/org.onaips.vnc/lib/
adb push libs/armeabi-v7a/androidvncserver /data/data/org.onaips.vnc/
