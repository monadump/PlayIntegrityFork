package es.chiteroman.playintegrityfix;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.Map;

public final class Pixelify implements InvocationHandler {
    private final Object original;
    private final Map<String, Boolean> features;

    public Pixelify(Object original, Map<String, Boolean> features) {
        this.original = original;
        this.features = features;
    }

    @Override
    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
        if (args != null && args.length > 0 && args[0] instanceof String
                && "hasSystemFeature".equals(method.getName())) {
            Object result = invokeOriginal(method, args);
            Boolean override = features.get((String) args[0]);
            if (override != null) {
                boolean real = Boolean.TRUE.equals(result);
                if (real != override) {
                    EntryPointPhotos.LOG("[" + args[0] + "]: " + real + " -> " + override + " (Photos config)");
                } else if (EntryPointPhotos.getVerboseLogs() > 1) {
                    EntryPointPhotos.LOG("[" + args[0] + "]: " + real + " (unchanged, Photos config)");
                }
                return override;
            }
            if (EntryPointPhotos.getVerboseLogs() > 1) {
                EntryPointPhotos.LOG("hasSystemFeature('" + args[0] + "'): passthrough -> " + result);
            }
            return result;
        }
        return invokeOriginal(method, args);
    }

    private Object invokeOriginal(Method method, Object[] args) throws Throwable {
        try {
            return method.invoke(original, args);
        } catch (InvocationTargetException e) {
            Throwable cause = e.getCause();
            if ("setComponentEnabledSetting".equals(method.getName())
                    && cause instanceof IllegalArgumentException) {
                String message = cause.getMessage();
                if (message != null
                        && message.contains("Component class")
                        && message.contains("does not exist in com.google.android.apps.photos")) {
                    // new Photos versions request components that are absent from the installed APK.
                    return null;
                }
            }
            throw cause != null ? cause : e;
        }
    }
}
