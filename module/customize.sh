# Allow a scripts-only mode for older Android (<10) which may not require the Zygisk components
if [ -f /data/adb/modules/integrity-box/scripts-only-mode ]; then
    ui_print "! Installing global scripts only; Zygisk attestation fallback and device spoofing disabled"
    touch $MODPATH/scripts-only-mode
    sed -i 's/\(description=\)\(.*\)/\1[Scripts-only mode] \2/' $MODPATH/module.prop
    [ -f /data/adb/modules/integrity-box/uninstall.sh ] && sh /data/adb/modules/integrity-box/uninstall.sh
    rm -rf $MODPATH/action.sh $MODPATH/app_replace_list.txt \
        $MODPATH/autopif4.sh $MODPATH/classes.dex \
        $MODPATH/common_setup.sh $MODPATH/custom.app_replace_list.txt \
        $MODPATH/custom.pif.json $MODPATH/config.meow  \
        $MODPATH/example.pif.json $MODPATH/example.pif.prop $MODPATH/migrate.sh \
        $MODPATH/pif.json $MODPATH/pif.prop $MODPATH/zygisk \
        /data/adb/modules/integrity-box/custom.app_replace_list.txt \
        /data/adb/modules/integrity-box/custom.pif.json \
        /data/adb/modules/integrity-box/config.meow \
        /data/adb/modules/integrity-box/skippersistprop \
        /data/adb/modules/integrity-box/system \
        /data/adb/modules/integrity-box/uninstall.sh
fi

# Copy any disabled app files to updated module
if [ -d /data/adb/modules/integrity-box/system ]; then
    ui_print "- Restoring disabled ROM apps configuration"
    cp -afL /data/adb/modules/integrity-box/system $MODPATH
fi

# Copy any supported custom files to updated module
for FILE in custom.app_replace_list.txt custom.pif.json config.meow skipdelprop skippersistprop uninstall.sh; do
    if [ -f "/data/adb/modules/integrity-box/$FILE" ]; then
        ui_print "- Restoring $FILE"
        cp -af /data/adb/modules/integrity-box/$FILE $MODPATH/$FILE
    fi
done

# Warn if potentially conflicting modules are installed
if [ -d /data/adb/modules/MagiskHidePropsConf ]; then
    ui_print "! MagiskHidePropsConfig (MHPC) module may cause issues with PIF"
fi

# Run common tasks for installation and boot-time
if [ -d "$MODPATH/zygisk" ]; then
    . $MODPATH/common_func.sh
    . $MODPATH/common_setup.sh
fi

# Migrate custom.pif.json to latest defaults if needed
if [ -f "$MODPATH/custom.pif.json" ]; then
    if ! grep -q "api_level" $MODPATH/custom.pif.json || ! grep -q "verboseLogs" $MODPATH/custom.pif.json || ! grep -q "spoofVendingFinger" $MODPATH/custom.pif.json; then
        ui_print "- Running migration script on custom.pif.json:"
        ui_print " "
        chmod 755 $MODPATH/migrate.sh
        sh $MODPATH/migrate.sh --install --force --advanced $MODPATH/custom.pif.json
        ui_print " "
    fi
fi

# Clean up any leftover files from previous deprecated methods
rm -f /data/data/com.google.android.gms/cache/pif.prop /data/data/com.google.android.gms/pif.prop \
    /data/data/com.google.android.gms/cache/pif.json /data/data/com.google.android.gms/pif.json
