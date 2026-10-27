package es.chiteroman.playintegrityfix;

import android.os.Build;
import android.util.JsonReader;
import android.util.Log;

import org.lsposed.hiddenapibypass.HiddenApiBypass;

import java.io.IOException;
import java.io.StringReader;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.Proxy;
import java.util.HashMap;
import java.util.Map;

public final class EntryPointPhotos {
    private static Integer verboseLogs = 0;
    private static final Map<String, String> map = new HashMap<>();

    public static native void setFieldNative(Class<?> clazz, Field field, String type, Object value);

    public static void init(int logLevel, int spoofBuildVal, int spoofFeaturesVal) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            HiddenApiBypass.addHiddenApiExemptions("");
        }
        verboseLogs = logLevel;
        LOG("Photos spoof init; API=" + Build.VERSION.SDK_INT
                + ", build=" + spoofBuildVal + ", features=" + spoofFeaturesVal);
        if (spoofBuildVal > 0) spoofDevice();
        if (spoofFeaturesVal > 0) spoofSystemFeatures();
    }

    public static void receiveJson(String data) {
        try (JsonReader reader = new JsonReader(new StringReader(data))) {
            reader.beginObject();
            while (reader.hasNext()) {
                map.put(reader.nextName(), reader.nextString());
            }
            reader.endObject();
        } catch (IOException | IllegalStateException e) {
            LOG("Couldn't read JSON: " + e);
            map.clear();
        }
    }

    static void spoofDevice() {
        int applied = 0;
        for (Map.Entry<String, String> entry : map.entrySet()) {
            String key = entry.getKey();
            if (key.indexOf('.') >= 0 || key.indexOf('*') >= 0) continue; // feature, not a Build field
            setField(key, entry.getValue());
            applied++;
        }
        LOG("Photos Build spoofing applied to " + applied + " fields");
    }

    private static void spoofSystemFeatures() {
        try {
            Map<String, Boolean> features = new HashMap<>();
            for (Map.Entry<String, String> entry : map.entrySet()) {
                String key = entry.getKey();
                if (key.indexOf('.') >= 0) {
                    features.put(key, Boolean.parseBoolean(entry.getValue()));
                }
            }
            if (features.isEmpty()) {
                LOG("No feature overrides in photos config");
                return;
            }

            Class<?> activityThreadClass = Class.forName("android.app.ActivityThread");
            Method getPackageManager = activityThreadClass.getDeclaredMethod("getPackageManager");
            getPackageManager.setAccessible(true);
            Object real = getPackageManager.invoke(null);
            if (real == null) {
                LOG("Couldn't get IPackageManager");
                return;
            }

            Class<?> iPackageManagerClass = Class.forName("android.content.pm.IPackageManager");
            Object proxy = Proxy.newProxyInstance(
                    Pixelify.class.getClassLoader(),
                    new Class<?>[]{iPackageManagerClass},
                    new Pixelify(real, features));

            Field f = findField(activityThreadClass, "sPackageManager");
            f.setAccessible(true);
            f.set(null, proxy);

            LOG("Spoof hasSystemFeature done! (" + features.size() + " overrides)");
        } catch (Exception e) {
            LOG("Couldn't spoof system features: " + e);
        }
    }

    static Integer getVerboseLogs() {
        return verboseLogs;
    }

    private static Field findField(Class<?> startClass, String fieldName) throws NoSuchFieldException {
        Class<?> current = startClass;
        while (current != null && current != Object.class) {
            try {
                return current.getDeclaredField(fieldName);
            } catch (NoSuchFieldException ignored) {
                current = current.getSuperclass();
            }
        }
        throw new NoSuchFieldException("Field '" + fieldName + "' not found in class hierarchy of " + startClass.getName());
    }

    private static void setField(String name, String value) {
        if (value == null || value.isEmpty()) return;

        Field field;
        try {
            if (classContainsField(Build.class, name)) {
                field = Build.class.getDeclaredField(name);
            } else if (classContainsField(Build.VERSION.class, name)) {
                field = Build.VERSION.class.getDeclaredField(name);
            } else {
                if (verboseLogs > 1) LOG("[" + name + "]: not a Build field, skipping");
                return;
            }
        } catch (NoSuchFieldException e) {
            LOG("[" + name + "]: failed to find field: " + e);
            return;
        }

        field.setAccessible(true);
        try {
            Field accessFlags = Field.class.getDeclaredField("accessFlags");
            accessFlags.setAccessible(true);
            accessFlags.setInt(field, field.getModifiers() & ~Modifier.FINAL);
        } catch (Exception ignored) {
        }

        try {
            if (field.getType() == int.class) {
                setFieldNative(field.getDeclaringClass(), field, "int", Integer.valueOf(value.trim()));
            } else {
                setFieldNative(field.getDeclaringClass(), field, "java.lang.String", value);
            }
            if (verboseLogs > 0) LOG("[" + name + "]: -> " + value);
        } catch (Exception e) {
            LOG("[" + name + "]: couldn't set: " + e);
        }
    }

    private static boolean classContainsField(Class<?> clazz, String fieldName) {
        for (Field f : clazz.getDeclaredFields()) {
            if (f.getName().equals(fieldName)) return true;
        }
        return false;
    }

    static void LOG(String msg) {
        Log.d("PIF/Java:Photos", msg);
    }
}
