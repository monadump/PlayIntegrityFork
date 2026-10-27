package es.chiteroman.playintegrityfix;

import android.os.Build;
import android.util.JsonReader;
import android.util.Log;

import org.lsposed.hiddenapibypass.HiddenApiBypass;

import java.io.IOException;
import java.io.StringReader;
import java.lang.reflect.Field;
import java.util.HashMap;
import java.util.Map;
import java.util.TimeZone;

public final class EntryPointApps {
    private static Integer verboseLogs = 0;
    private static final Map<String, String> map = new HashMap<>();

    public static void init(int logLevel, String data) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            HiddenApiBypass.addHiddenApiExemptions("");
        }
        verboseLogs = logLevel;
        map.clear();
        LOG("Starting per-app profile injection; Android API=" + Build.VERSION.SDK_INT
                + ", log level=" + verboseLogs);
        receiveJson(data);
        LOG("Parsed " + map.size() + " profile Build fields");
        applyProfileEnvironment();
        if (verboseLogs > 2) logFields();
        spoofDevice();
    }

    public static void receiveJson(String data) {
        try (JsonReader reader = new JsonReader(new StringReader(data))) {
            reader.beginObject();
            while (reader.hasNext()) {
                String key = reader.nextName();
                if (key.equals("verboseLogs") || key.equals("spoofBuild") || key.equals("spoofProps")
                        || key.equals("spoofProvider") || key.equals("spoofSignature")
                        || key.equals("spoofVendingFinger") || key.equals("spoofVendingSdk")
                        || key.equals("spoofPixel") || key.equals("spoofApps")) {
                    reader.skipValue();
                    continue;
                }
                map.put(key, reader.nextString());
            }
            reader.endObject();
        } catch (IOException | IllegalStateException e) {
            LOG("Couldn't read JSON: " + e);
            map.clear();
        }
    }


    private static void applyProfileEnvironment() {
        String timezone = map.get("persist.sys.timezone");
        if (timezone != null && !timezone.isEmpty()) {
            TimeZone zone = TimeZone.getTimeZone(timezone);
            if (!"GMT".equals(zone.getID()) || "GMT".equals(timezone)) {
                TimeZone.setDefault(zone);
                LOG("[persist.sys.timezone]: -> " + zone.getID());
            } else {
                LOG("Invalid timezone in app profile: " + timezone);
            }
        }
    }

    static void spoofDevice() {
        int applied = 0;
        int unchanged = 0;
        int skipped = 0;
        int failed = 0;

        for (Map.Entry<String, String> entry : map.entrySet()) {
            String key = entry.getKey();
            if (key.equals("persist.sys.timezone")) continue;
            String result = setField(key, entry.getValue());
            if ("applied".equals(result)) applied++;
            else if ("unchanged".equals(result)) unchanged++;
            else if ("skipped".equals(result)) skipped++;
            else failed++;
        }

        LOG("Per-app spoofing summary: applied=" + applied + ", unchanged=" + unchanged
                + ", skipped=" + skipped + ", failed=" + failed
                + ", profileFields=" + map.size());
    }

    private static String setField(String name, String value) {
        if (value == null || value.isEmpty()) {
            LOG("[" + name + "]: skipped (empty value)");
            return "skipped";
        }

        if (name.equals("SDK_INT") ||
                name.equals("SDK_INT_FULL") ||
                name.equals("DEVICE_INITIAL_SDK_INT") ||
                name.equals("FIRST_SDK_INT") ||
                name.equals("PREVIEW_SDK_INT") ||
                name.equals("RESOURCES_SDK_INT") ||
                name.equals("CPU_ABI") ||
                name.equals("CPU_ABI2") ||
                name.equals("SUPPORTED_ABIS") ||
                name.equals("SUPPORTED_32_BIT_ABIS") ||
                name.equals("SUPPORTED_64_BIT_ABIS") ||
                name.equals("ALL_CODENAMES")) {
            LOG("[" + name + "]: skipped (runtime-sensitive field; changing this can make the process inconsistent)");
            return "skipped";
        }

        Field field = null;
        String oldValue = null;
        Object newValue = null;

        try {
            if (classContainsField(Build.class, name)) {
                field = Build.class.getDeclaredField(name);
            } else if (classContainsField(Build.VERSION.class, name)) {
                field = Build.VERSION.class.getDeclaredField(name);
            } else {
                // SOC_* fields were added in Android 12 (API 31). Older releases do not
                // expose them on android.os.Build, so reflection cannot spoof them there.
                if ((name.equals("SOC_MANUFACTURER") || name.equals("SOC_MODEL"))
                        && Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
                    LOG("[" + name + "]: skipped (requires Android 12/API 31 or newer; current API="
                            + Build.VERSION.SDK_INT + ")");
                } else {
                    // Properties such as DESCRIPTION, FLAVOR, and CHARACTERISTICS do not
                    // correspond to fields on android.os.Build on this runtime.
                    LOG("[" + name + "]: skipped (field is not exposed by Build/Build.VERSION on this Android release)");
                }
                return "skipped";
            }
        } catch (NoSuchFieldException e) {
            LOG("[" + name + "]: failed to find field: " + e);
            return "failed";
        }

        field.setAccessible(true);
        try {
            Field accessFlagsField = Field.class.getDeclaredField("accessFlags");
            accessFlagsField.setAccessible(true);
            accessFlagsField.setInt(field, field.getModifiers() & ~java.lang.reflect.Modifier.FINAL);
        } catch (NoSuchFieldException | IllegalAccessException e) {
            LOG("[" + name + "]: warning: couldn't modify reflection access flags: " + e);
        }

        try {
            oldValue = String.valueOf(field.get(null));
        } catch (IllegalAccessException e) {
            LOG("[" + name + "]: failed to read current value: " + e);
            return "failed";
        }

        if (value.equals(oldValue)) {
            LOG("[" + name + "]: unchanged (" + value + ")");
            return "unchanged";
        }

        Class<?> fieldType = field.getType();
        try {
            if (fieldType == String.class) {
                newValue = value;
            } else if (fieldType == int.class) {
                newValue = Integer.parseInt(value);
            } else if (fieldType == long.class) {
                newValue = Long.parseLong(value);
            } else if (fieldType == boolean.class) {
                if (!"true".equalsIgnoreCase(value) && !"false".equalsIgnoreCase(value)) {
                    LOG("[" + name + "]: failed (invalid boolean value '" + value + "')");
                    return "failed";
                }
                newValue = Boolean.parseBoolean(value);
            } else {
                LOG("[" + name + "]: failed (unsupported field type " + fieldType.getName() + ")");
                return "failed";
            }
        } catch (NumberFormatException e) {
            LOG("[" + name + "]: failed to parse '" + value + "' as " + fieldType.getName());
            return "failed";
        }

        try {
            setFieldNative(field.getDeclaringClass(), field, fieldType.getName(), newValue);
            if (fieldType == String.class) {
                String actual = String.valueOf(field.get(null));
                if (!value.equals(actual)) {
                    LOG("[" + name + "]: verification failed; requested='" + value + "', actual='" + actual + "'");
                    return "failed";
                }
            } else if (!String.valueOf(field.get(null)).equals(String.valueOf(newValue))) {
                LOG("[" + name + "]: verification failed; requested='" + newValue
                        + "', actual='" + String.valueOf(field.get(null)) + "'");
                return "failed";
            }
        } catch (Exception e) {
            LOG("[" + name + "]: native write/verification failed: " + e);
            return "failed";
        }

        LOG("[" + name + "]: applied '" + oldValue + "' -> '" + value + "'");
        return "applied";
    }

    private static boolean classContainsField(Class<?> className, String fieldName) {
        for (Field field : className.getDeclaredFields()) {
            if (field.getName().equals(fieldName)) return true;
        }
        return false;
    }

    private static native void setFieldNative(Class<?> targetClass, Field field, String type, Object value);

    private static String logParseField(Field field) {
        Object value = null;
        String type = field.getType().getName();
        String name = field.getName();
        try {
            value = field.get(null);
        } catch (IllegalAccessException | NullPointerException e) {
            return "Couldn't access '" + name + "' value: " + e;
        }
        return "<" + type + "> " + name + ": " + String.valueOf(value);
    }

    private static void logFields() {
        for (Field field : Build.class.getDeclaredFields()) {
            LOG("Build " + logParseField(field));
        }
        for (Field field : Build.VERSION.class.getDeclaredFields()) {
            LOG("Build.VERSION " + logParseField(field));
        }
    }

    static void LOG(String msg) {
        Log.d("PIF/Java:APPS", msg);
    }
}
