#include <android/log.h>
#include <cstdint>
#include <sys/system_properties.h>
#include <sys/stat.h>
#include <unistd.h>

#include "zygisk.hpp"
#include "json/single_include/nlohmann/json.hpp"
#include "property_virtualizer.hpp"
#include "pif_config.hpp"

#if defined(__aarch64__) || defined(__arm__)
#include "shadowhook.h"
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "PIF/Native", __VA_ARGS__)

#define MODULE_DIR "/data/adb/modules/integrity-box/"

#define DEX_FILE_PATH MODULE_DIR "classes.dex"
#define PROP_FILE_PATH MODULE_DIR "pif.prop"
#define CUSTOM_PROP_FILE_PATH MODULE_DIR "config.meow"
#define JSON_FILE_PATH MODULE_DIR "pif.json"
#define CUSTOM_JSON_FILE_PATH MODULE_DIR "custom.pif.json"
#define APPS_FILE_PATH MODULE_DIR "apps.txt"
#define APP_PROFILE_FILE_PATH MODULE_DIR "app_profile.prop"
#define PHOTOS_PROP_FILE_PATH MODULE_DIR "sailfish.prop"
#define STOP_FILE MODULE_DIR "stop"
#define STOP_FILE_BOX_BRAIN "/data/adb/Box-Brain/PIF/stop"

#define PHOTOS_PACKAGE "com.google.android.apps.photos"
#define VENDING_PACKAGE "com.android.vending"
#define DROIDGUARD_PACKAGE "com.google.android.gms.unstable"

#define ENTRY_CLASS_DROIDGUARD "es.chiteroman.playintegrityfix.EntryPoint"
#define ENTRY_CLASS_VENDING "es.chiteroman.playintegrityfix.EntryPointVending"
#define ENTRY_CLASS_PHOTOS "es.chiteroman.playintegrityfix.EntryPointPhotos"
#define ENTRY_CLASS_APPS "es.chiteroman.playintegrityfix.EntryPointApps"

static constexpr size_t MAX_DEX_PAYLOAD = 64U * 1024U * 1024U;
static constexpr size_t MAX_CONFIG_PAYLOAD = 16U * 1024U * 1024U;
static constexpr size_t MAX_APP_PROFILE_PAYLOAD = 16U * 1024U * 1024U;

using PropMap = std::map<std::string, std::string, std::less<>>;

static int verboseLogs = 0;
static int spoofBuild = 1;
static int spoofProps = 1;
static int spoofProvider = 1;
static int spoofSignature = 0;
static int spoofVendingFinger = 0;
static int spoofVendingSdk = 0;
static int spoofPatch = 0;
static int spoofPixel = 0;
static int spoofApps = 0;
static int spoofPhotos = 0;

static PropMap jsonProps;
static PropMap photosJsonProps;
static PropMap appJsonProps;
static nlohmann::json photosJson = nlohmann::json::object();
static nlohmann::json appJson = nlohmann::json::object();
static std::string appProfileData;

// ---------------------------------------------------------------------------
// Generic helpers
// ---------------------------------------------------------------------------

static bool readFully(int fd, void *buffer, size_t size) {
    auto *bytes = static_cast<unsigned char *>(buffer);
    size_t offset = 0;
    while (offset < size) {
        const ssize_t count = read(fd, bytes + offset, size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

static bool writeFully(int fd, const void *buffer, size_t size) {
    const auto *bytes = static_cast<const unsigned char *>(buffer);
    size_t offset = 0;
    while (offset < size) {
        const ssize_t count = write(fd, bytes + offset, size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

class ScopedFd {
public:
    explicit ScopedFd(int fd) : fd_(fd) {}
    ~ScopedFd() { if (fd_ >= 0) close(fd_); }
    ScopedFd(const ScopedFd &) = delete;
    ScopedFd &operator=(const ScopedFd &) = delete;
    int get() const { return fd_; }

private:
    int fd_;
};

static std::string_view trim(std::string_view text) {
    constexpr std::string_view whitespace = " \t\r\n";
    const size_t first = text.find_first_not_of(whitespace);
    if (first == std::string_view::npos) return {};
    const size_t last = text.find_last_not_of(whitespace);
    return text.substr(first, last - first + 1);
}

static int parseIntOrKeep(const std::string &value, int current, const char *key) {
    errno = 0;
    char *end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);

    if (value.empty() || errno == ERANGE || end == value.c_str() || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX) {
        LOGD("Invalid integer for %s; keeping previous value %d", key, current);
        return current;
    }
    return static_cast<int>(parsed);
}

static bool readFile(const char *path, std::vector<char> &out, size_t maxSize) {
    out.clear();
    FILE *file = fopen(path, "rb");
    if (file == nullptr) return false;

    bool ok = false;
    if (fseek(file, 0, SEEK_END) == 0) {
        const long length = ftell(file);
        if (length >= 0 && static_cast<unsigned long long>(length) <= maxSize &&
            fseek(file, 0, SEEK_SET) == 0) {
            out.resize(static_cast<size_t>(length));
            ok = out.empty() || fread(out.data(), 1, out.size(), file) == out.size();
        }
    }
    fclose(file);
    if (!ok) out.clear();
    return ok;
}

using PropLines = std::vector<std::pair<std::string, std::string>>;

// Parses "name=value" lines. '#' starts a comment, blank and malformed lines are skipped.
// The final line is honored even without a trailing newline.
static PropLines parsePropLines(std::string_view text, bool skipEmptyValues) {
    PropLines result;
    size_t begin = 0;
    while (begin <= text.size()) {
        size_t end = text.find('\n', begin);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = trim(text.substr(begin, end - begin));
        begin = end + 1;

        if (line.empty() || line.front() == '#') continue;

        const size_t delimiter = line.find('=');
        if (delimiter == std::string_view::npos) {
            LOGD("Invalid prop entry, skipping");
            continue;
        }

        const std::string_view name = trim(line.substr(0, delimiter));
        std::string_view value = line.substr(delimiter + 1);
        const size_t comment = value.find('#');
        if (comment != std::string_view::npos) value = value.substr(0, comment);
        value = trim(value);

        if (name.empty() || (skipEmptyValues && value.empty())) continue;
        result.emplace_back(std::string(name), std::string(value));
    }
    return result;
}

// Accepts JSON (comments allowed) or prop syntax. Returns an empty object on failure.
static nlohmann::json parseConfig(const std::string &text, bool skipEmptyValues) {
    if (nlohmann::json::accept(text, true)) {
        auto parsed = nlohmann::json::parse(text, nullptr, false, true);
        if (parsed.is_object()) return parsed;
        return nlohmann::json::object();
    }

    LOGD("Converting config from prop format to JSON format");
    nlohmann::json result = nlohmann::json::object();
    for (auto &[name, value] : parsePropLines(text, skipEmptyValues))
        result[name] = std::move(value);
    return result;
}

static bool isPackageInAppsList(const std::string &pkgName) {
    FILE *appsFile = fopen(APPS_FILE_PATH, "r");
    if (appsFile == nullptr) return false;

    bool found = false;
    char line[512];
    while (!found && fgets(line, sizeof(line), appsFile)) {
        const std::string_view entry = trim(line);
        found = !entry.empty() && entry == pkgName;
    }

    fclose(appsFile);
    return found;
}

static bool isPaused() {
    struct stat st {};
    return stat(STOP_FILE, &st) == 0 || stat(STOP_FILE_BOX_BRAIN, &st) == 0;
}

// ---------------------------------------------------------------------------
// Property spoofing
// ---------------------------------------------------------------------------

#if defined(__aarch64__) || defined(__arm__)
typedef void (*T_Callback)(void *, const char *, const char *, uint32_t);

struct CallbackContext {
    T_Callback callback;
    void *originalCookie;
};

// Patterns of the form "*suffix", resolved once before the hook is installed.
static std::vector<std::pair<std::string, std::string>> wildcardProps;

static void modify_callback(void *cookie, const char *name, const char *value, uint32_t serial) {
    auto *context = static_cast<CallbackContext *>(cookie);
    if (context == nullptr || context->callback == nullptr) return;

    const char *replacement = value;
    if (name != nullptr && value != nullptr) {
        const std::string_view prop(name);
        const auto exact = jsonProps.find(prop);
        if (exact != jsonProps.end()) {
            replacement = exact->second.c_str();
        } else {
            for (const auto &[suffix, spoofed] : wildcardProps) {
                if (prop.ends_with(suffix)) {
                    replacement = spoofed.c_str();
                    break;
                }
            }
        }

        if (verboseLogs > 0 && strcmp(value, replacement) != 0) {
            LOGD("[%s]: %s -> %s", name, value, replacement);
        } else if (verboseLogs > 99) {
            LOGD("[%s]: %s (unchanged)", name, value);
        }
    }

    context->callback(context->originalCookie, name, replacement, serial);
}

static void (*o_system_property_read_callback)(const prop_info *, T_Callback, void *);

static void my_system_property_read_callback(const prop_info *pi, T_Callback callback, void *cookie) {
    if (o_system_property_read_callback == nullptr) return;
    if (pi == nullptr || callback == nullptr || cookie == nullptr) {
        o_system_property_read_callback(pi, callback, cookie);
        return;
    }

    CallbackContext context{callback, cookie};
    o_system_property_read_callback(pi, modify_callback, &context);
}

extern "C" int sh_linker_init(void) {
    return 0;
}

static void doHook() {
    wildcardProps.clear();
    for (const auto &[pattern, spoofed] : jsonProps) {
        if (pattern.size() > 1 && pattern.front() == '*')
            wildcardProps.emplace_back(pattern.substr(1), spoofed);
    }

    shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);
    void *symbol = dlsym(RTLD_DEFAULT, "__system_property_read_callback");
    if (symbol == nullptr) {
        LOGD("Couldn't find '__system_property_read_callback' via dlsym");
        return;
    }

    void *handle = shadowhook_hook_sym_addr(
            symbol,
            reinterpret_cast<void *>(my_system_property_read_callback),
            reinterpret_cast<void **>(&o_system_property_read_callback));
    if (handle == nullptr) {
        LOGD("Couldn't hook '__system_property_read_callback'. Error: %d", shadowhook_get_errno());
        return;
    }

    LOGD("Installed fallback property callback hook at %p", handle);
}
#else
static void doHook() {
    LOGD("Property callback hook is unavailable on this ABI; using property-area virtualization only");
}
#endif

// Fills BRAND/PRODUCT/DEVICE/... from "brand/product/device:release/id/incremental:type/tags".
static void expandFingerprint(std::map<std::string, std::string> &propMap,
                              const std::string &fingerprint) {
    if (fingerprint.empty()) return;

    static constexpr const char *fieldNames[] = {
        "BRAND", "PRODUCT", "DEVICE", "RELEASE", "ID", "INCREMENTAL", "TYPE", "TAGS"
    };

    size_t index = 0;
    size_t begin = 0;
    while (index < std::size(fieldNames)) {
        const size_t end = fingerprint.find_first_of("/:", begin);
        const std::string part = fingerprint.substr(
                begin, end == std::string::npos ? std::string::npos : end - begin);
        propMap.emplace(fieldNames[index++], part);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
}

static std::string jsonString(const nlohmann::json &source, const char *key) {
    const auto it = source.find(key);
    return it != source.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// mergeSource: also expose every string entry of `source` as a spoofed property key.
static bool installVirtualizer(const PropMap &props, const nlohmann::json &source, bool mergeSource) {
    pif::Config config;
    config.debug = verboseLogs > 1;

    for (const auto &[key, value] : props) config.propMap[key] = value;

    if (mergeSource) {
        for (const auto &item : source.items()) {
            if (item.value().is_string())
                config.propMap[item.key()] = item.value().get<std::string>();
        }
    }

    expandFingerprint(config.propMap, jsonString(source, "FINGERPRINT"));

    config.buildId = jsonString(source, "ID");
    config.securityPatch = jsonString(source, "SECURITY_PATCH");
    const std::string initialSdk = jsonString(source, "DEVICE_INITIAL_SDK_INT");
    if (!initialSdk.empty()) config.deviceInitialSdkInt = initialSdk;

    const bool installed = pif::property_virtualizer::install(config);
    if (!installed) LOGD("Property-area virtualization failed");
    return installed;
}

// ---------------------------------------------------------------------------
// Profiles
// ---------------------------------------------------------------------------

static bool loadAppProfile() {
    static constexpr struct {
        const char *property;
        const char *field;
    } mappings[] = {
        {"ro.product.manufacturer", "MANUFACTURER"},
        {"ro.product.brand", "BRAND"},
        {"ro.product.model", "MODEL"},
        {"ro.product.name", "PRODUCT"},
        {"ro.product.device", "DEVICE"},
        {"ro.product.board", "BOARD"},
        {"ro.build.fingerprint", "FINGERPRINT"},
        {"ro.build.version.release", "RELEASE"},
        {"ro.build.version.sdk", "SDK_INT"},
        {"ro.build.version.preview_sdk", "PREVIEW_SDK_INT"},
        {"ro.build.id", "ID"},
        {"ro.build.version.incremental", "INCREMENTAL"},
        {"ro.build.type", "TYPE"},
        {"ro.build.tags", "TAGS"},
        {"ro.build.version.security_patch", "SECURITY_PATCH"},
        {"ro.build.version.codename", "CODENAME"},
        {"ro.build.version.all_codenames", "ALL_CODENAMES"},
        {"ro.build.version.base_os", "BASE_OS"},
        {"ro.build.display.id", "DISPLAY"},
        {"ro.build.user", "USER"},
        {"ro.build.host", "HOST"},
        {"ro.bootloader", "BOOTLOADER"},
        {"ro.hardware", "HARDWARE"},
        {"ro.soc.manufacturer", "SOC_MANUFACTURER"},
        {"ro.soc.model", "SOC_MODEL"},
    };

    appJson = nlohmann::json::object();
    appJsonProps.clear();

    if (appProfileData.empty()) {
        LOGD("App profile payload is empty");
        return false;
    }

    for (auto &[name, value] : parsePropLines(appProfileData, true)) {
        const auto mapping = std::find_if(std::begin(mappings), std::end(mappings),
                                          [&](const auto &m) { return name == m.property; });
        if (mapping != std::end(mappings)) {
            appJson[mapping->field] = value;
            if (verboseLogs > 0)
                LOGD("App profile mapping: %s -> Build.%s", name.c_str(), mapping->field);
        } else if (name != "persist.sys.timezone" && verboseLogs > 0) {
            LOGD("App profile property has no Build mapping: %s (left unchanged)", name.c_str());
        }
        appJsonProps[std::move(name)] = std::move(value);
    }

    const auto timezone = appJsonProps.find("persist.sys.timezone");
    if (timezone != appJsonProps.end()) appJson["persist.sys.timezone"] = timezone->second;

    LOGD("Loaded app profile: %d properties, %d JSON entries",
         static_cast<int>(appJsonProps.size()), static_cast<int>(appJson.size()));
    return !appJsonProps.empty();
}

static bool hasUppercase(const std::string &value) {
    return std::any_of(value.begin(), value.end(),
                       [](unsigned char c) { return std::isupper(c) != 0; });
}

// Splits the Photos config into system-property entries and Java Build fields.
static void readPhotosJson() {
    photosJsonProps.clear();
    for (auto it = photosJson.begin(); it != photosJson.end();) {
        const std::string &key = it.key();
        const bool hasDot = key.find('.') != std::string::npos;
        const bool hasStar = key.find('*') != std::string::npos;
        const bool nonEmptyString = it.value().is_string() && !it.value().get<std::string>().empty();

        if (hasStar || (hasDot && !hasUppercase(key))) {
            if (nonEmptyString) photosJsonProps[key] = it.value().get<std::string>();
            it = photosJson.erase(it);
        } else if (hasDot && !nonEmptyString) {
            it = photosJson.erase(it);
        } else {
            ++it;
        }
    }
    LOGD("Photos config: %d props, %d Java entries",
         static_cast<int>(photosJsonProps.size()), static_cast<int>(photosJson.size()));
}

// ---------------------------------------------------------------------------
// JNI helpers
// ---------------------------------------------------------------------------

static bool checkJniException(JNIEnv *env, const char *stage) {
    if (env != nullptr && env->ExceptionCheck()) {
        LOGD("JNI: exception during %s; aborting this operation", stage);
        env->ExceptionClear();
        return true;
    }
    return false;
}

class ScopedLocalFrame {
public:
    ScopedLocalFrame(JNIEnv *env, jint capacity) : env_(env) {
        active_ = env_ != nullptr && env_->PushLocalFrame(capacity) == JNI_OK;
        if (!active_) checkJniException(env_, "PushLocalFrame");
    }

    ~ScopedLocalFrame() {
        if (active_) env_->PopLocalFrame(nullptr);
    }

    ScopedLocalFrame(const ScopedLocalFrame &) = delete;
    ScopedLocalFrame &operator=(const ScopedLocalFrame &) = delete;

    bool valid() const { return active_; }

private:
    JNIEnv *env_;
    bool active_ = false;
};

class ScopedUtf {
public:
    ScopedUtf(JNIEnv *env, jstring str)
            : env_(env), str_(str),
              chars_(str != nullptr ? env->GetStringUTFChars(str, nullptr) : nullptr) {}

    ~ScopedUtf() {
        if (chars_ != nullptr) env_->ReleaseStringUTFChars(str_, chars_);
    }

    ScopedUtf(const ScopedUtf &) = delete;
    ScopedUtf &operator=(const ScopedUtf &) = delete;

    const char *get() const { return chars_; }

private:
    JNIEnv *env_;
    jstring str_;
    const char *chars_;
};

static void setFieldNative(JNIEnv *env, jclass /* entryPoint */, jclass targetClass,
                           jobject fieldObj, jstring typeObj, jobject valueObj) {
    if (env == nullptr || targetClass == nullptr || fieldObj == nullptr || typeObj == nullptr)
        return;

    jfieldID fieldID = env->FromReflectedField(fieldObj);
    if (checkJniException(env, "FromReflectedField") || fieldID == nullptr) return;

    ScopedUtf type(env, typeObj);
    if (checkJniException(env, "GetStringUTFChars") || type.get() == nullptr) return;
    const std::string_view typeName(type.get());

    if (typeName == "java.lang.String" || typeName == "[Ljava.lang.String;") {
        env->SetStaticObjectField(targetClass, fieldID, valueObj);
        checkJniException(env, "SetStaticObjectField");
        return;
    }
    if (valueObj == nullptr) return;

    const char *wrapperName = nullptr;
    const char *methodName = nullptr;
    const char *signature = nullptr;
    if (typeName == "int") {
        wrapperName = "java/lang/Integer", methodName = "intValue", signature = "()I";
    } else if (typeName == "long") {
        wrapperName = "java/lang/Long", methodName = "longValue", signature = "()J";
    } else if (typeName == "boolean") {
        wrapperName = "java/lang/Boolean", methodName = "booleanValue", signature = "()Z";
    } else {
        return;
    }

    jclass wrapperClass = env->FindClass(wrapperName);
    if (checkJniException(env, "FindClass(wrapper)") || wrapperClass == nullptr) return;

    jmethodID unbox = env->GetMethodID(wrapperClass, methodName, signature);
    env->DeleteLocalRef(wrapperClass);
    if (checkJniException(env, "GetMethodID(unbox)") || unbox == nullptr) return;

    if (typeName == "int") {
        const jint value = env->CallIntMethod(valueObj, unbox);
        if (!checkJniException(env, "Integer.intValue"))
            env->SetStaticIntField(targetClass, fieldID, value);
    } else if (typeName == "long") {
        const jlong value = env->CallLongMethod(valueObj, unbox);
        if (!checkJniException(env, "Long.longValue"))
            env->SetStaticLongField(targetClass, fieldID, value);
    } else {
        const jboolean value = env->CallBooleanMethod(valueObj, unbox);
        if (!checkJniException(env, "Boolean.booleanValue"))
            env->SetStaticBooleanField(targetClass, fieldID, value);
    }
    checkJniException(env, "SetStaticField");
}

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

struct PayloadHeader {
    int64_t dexSize;
    int64_t configSize;
    int64_t appProfileSize;
    int64_t photosConfigSize;
};

static bool inRange(int64_t value, int64_t min, size_t max) {
    return value >= min && static_cast<uint64_t>(value) <= max;
}

class PlayIntegrityFix : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        resolveNames(args);

        const bool isPhotos = pkgName == PHOTOS_PACKAGE;
        isDroidGuardOrVending = processName == DROIDGUARD_PACKAGE || pkgName == VENDING_PACKAGE;

        // Match apps by package, not process name, so secondary app processes use
        // the same profile without admitting unrelated GMS processes.
        isTargetApp = !pkgName.empty() && isPackageInAppsList(pkgName);

        if (!isDroidGuardOrVending && !isTargetApp && !isPhotos) {
            // Secondary Google Play Services processes are skipped on purpose; say so once per
            // process so the main/DroidGuard/other-GMS split is visible in a capture.
            if (pkgName.rfind("com.google.android.gms", 0) == 0) {
                LOGD("ZYGISK: pid=%d uid=%d pkg=%s proc=%s: not a target, unloading",
                     static_cast<int>(getpid()), static_cast<int>(args->uid),
                     pkgName.c_str(), processName.c_str());
            }
            unload();
            return;
        }

        LOGD("ZYGISK: pid=%d uid=%d pkg=%s proc=%s role=%s",
             static_cast<int>(getpid()), static_cast<int>(args->uid),
             pkgName.c_str(), processName.c_str(),
             processName == DROIDGUARD_PACKAGE ? "droidguard" :
             pkgName == VENDING_PACKAGE ? "vending" :
             isTargetApp ? "app" : "photos");

        api->setOption(zygisk::FORCE_DENYLIST_UNMOUNT);

        if (isDroidGuardOrVending && isPaused()) {
            LOGD("ZYGISK: Injection is paused by user, skipping spoofing for %s", pkgName.c_str());
            unload();
            return;
        }

        if (!fetchPayload()) {
            releasePayload();
            unload();
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (dexVector.empty() || json.empty()) return;

        readJson();

        if (pkgName == VENDING_PACKAGE)
            spoofBuild = spoofProps = spoofProvider = spoofSignature = 0;
        else
            spoofVendingFinger = spoofVendingSdk = spoofPatch = spoofPixel = 0;

        if (isTargetApp && spoofApps > 0) {
            if (hasPairIpCoreLoaded()) {
                LOGD("PairIP detected in target process; skipping per-app spoofing");
            } else if (loadAppProfile()) {
                injectApps();
            } else {
                LOGD("App spoofing enabled but no valid app profile is available");
            }
        } else if (isDroidGuardOrVending) {
            if (isPaused()) {
                LOGD("Paused: stop flag exists, skipping injection for %s", pkgName.c_str());
            } else {
                spoofSystem();
            }
        } else if (pkgName == PHOTOS_PACKAGE) {
            spoofPhotosApp();
        }

        releasePayload();
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs *) override {
        unload();
    }

private:
    enum class Pop { Absent, Invalid, Ok };

    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;

    std::vector<char> dexVector;
    std::string photosConfig;
    nlohmann::json json;
    std::string pkgName;
    std::string processName;

    std::string vendingFingerprintValue;
    std::string manufacturerValue;
    std::string brandValue;
    std::string modelValue;
    std::string productValue;
    std::string deviceValue;
    std::string idValue;
    std::string incrementalValue;
    std::string typeValue;
    std::string tagsValue;
    std::string releaseValue;
    std::string securityPatchValue;

    bool isTargetApp = false;
    bool isDroidGuardOrVending = false;

    void unload() {
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

    // The DEX is only cleared, not freed: the in-memory class loader keeps referencing it.
    void releasePayload() {
        dexVector.clear();
        json = nlohmann::json::object();
        appJson = nlohmann::json::object();
        photosJson = nlohmann::json::object();
        appJsonProps.clear();
        photosJsonProps.clear();
        std::string().swap(appProfileData);
        std::string().swap(photosConfig);
    }

    void resolveNames(zygisk::AppSpecializeArgs *args) {
        ScopedUtf process(env, args->nice_name);
        ScopedUtf dir(env, args->app_data_dir);

        processName = process.get() != nullptr ? process.get() : "";

        if (dir.get() != nullptr) {
            const std::string_view path(dir.get());
            const size_t slash = path.find_last_of('/');
            pkgName = std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
        } else {
            pkgName = processName.substr(0, processName.find(':'));
        }
    }

    bool fetchPayload() {
        ScopedFd fd(api->connectCompanion());
        if (fd.get() < 0) {
            LOGD("Couldn't connect to companion");
            return false;
        }

        PayloadHeader header{};
        if (!readFully(fd.get(), &header, sizeof(header))) {
            LOGD("Couldn't read companion header");
            return false;
        }

        if (!inRange(header.dexSize, 1, MAX_DEX_PAYLOAD) ||
            !inRange(header.configSize, 1, MAX_CONFIG_PAYLOAD) ||
            !inRange(header.appProfileSize, 0, MAX_APP_PROFILE_PAYLOAD) ||
            !inRange(header.photosConfigSize, 0, MAX_CONFIG_PAYLOAD)) {
            LOGD("Companion returned invalid payload sizes (dex=%lld, config=%lld, profile=%lld, photos=%lld)",
                 static_cast<long long>(header.dexSize), static_cast<long long>(header.configSize),
                 static_cast<long long>(header.appProfileSize),
                 static_cast<long long>(header.photosConfigSize));
            return false;
        }

        LOGD("Companion payload: dex=%lld, config=%lld, profile=%lld, photos=%lld bytes",
             static_cast<long long>(header.dexSize), static_cast<long long>(header.configSize),
             static_cast<long long>(header.appProfileSize),
             static_cast<long long>(header.photosConfigSize));

        std::string configString;
        if (!readBlob(fd.get(), header.dexSize, dexVector) ||
            !readBlob(fd.get(), header.configSize, configString) ||
            !readBlob(fd.get(), header.appProfileSize, appProfileData) ||
            !readBlob(fd.get(), header.photosConfigSize, photosConfig)) {
            LOGD("Couldn't read companion payload");
            return false;
        }

        json = parseConfig(configString, false);
        return true;
    }

    template <typename Container>
    static bool readBlob(int fd, int64_t size, Container &out) {
        out.resize(static_cast<size_t>(size));
        return size == 0 || readFully(fd, out.data(), out.size());
    }

    static bool hasPairIpCoreLoaded() {
        FILE *maps = fopen("/proc/self/maps", "r");
        if (maps == nullptr) return false;

        char line[1024];
        bool found = false;
        while (!found && fgets(line, sizeof(line), maps))
            found = strstr(line, "libpairipcore.so") != nullptr;

        fclose(maps);
        return found;
    }

    // --- config parsing ----------------------------------------------------

    Pop popString(const char *key, std::string &out) {
        const auto it = json.find(key);
        if (it == json.end()) return Pop::Absent;

        Pop result = Pop::Invalid;
        if (it->is_string()) {
            out = it->get<std::string>();
            if (!out.empty()) result = Pop::Ok;
        }
        json.erase(it);
        return result;
    }

    void popFlag(const char *key, int &target, const char *label) {
        std::string raw;
        switch (popString(key, raw)) {
            case Pop::Absent:
                return;
            case Pop::Invalid:
                LOGD("Error parsing %s!", key);
                return;
            case Pop::Ok:
                target = parseIntOrKeep(raw, target, key);
                if (label != nullptr && verboseLogs > 0)
                    LOGD("%s %s!", label, target > 0 ? "enabled" : "disabled");
                return;
        }
    }

    void popVendingFingerprint() {
        std::string raw;
        const Pop state = popString("spoofVendingFinger", raw);
        if (state == Pop::Absent) return;
        if (state == Pop::Invalid) {
            LOGD("Error parsing spoofVendingFinger!");
            return;
        }

        const std::string fingerprint = jsonString(json, "FINGERPRINT");
        if (raw.find_first_not_of("01") != std::string::npos) {
            spoofVendingFinger = 1;
            vendingFingerprintValue = raw;
        } else if (!fingerprint.empty()) {
            spoofVendingFinger = parseIntOrKeep(raw, spoofVendingFinger, "spoofVendingFinger");
            vendingFingerprintValue = fingerprint;
        } else {
            LOGD("Error parsing spoofVendingFinger or FINGERPRINT field!");
            return;
        }

        if (verboseLogs > 0)
            LOGD("Spoofing Fingerprint in Play Store %s!", spoofVendingFinger > 0 ? "enabled" : "disabled");
    }

    void readJson() {
        LOGD("JSON contains %d keys!", static_cast<int>(json.size()));

        popFlag("verboseLogs", verboseLogs, nullptr);
        if (verboseLogs > 0) LOGD("Verbose logging (level %d) enabled!", verboseLogs);

        popFlag("spoofVendingSdk", spoofVendingSdk, "Spoofing SDK Level in Play Store");
        popFlag("spoofPatch", spoofPatch, "Spoofing Android release and security patch");
        popVendingFingerprint();
        popFlag("spoofPixel", spoofPixel, "Spoofing Pixel Fields in Play Store");
        popFlag("spoofApps", spoofApps, "Per-app spoofing");
        popFlag("spoofPhotos", spoofPhotos, "Photos spoofing");

        const std::pair<const char *, std::string *> stringFields[] = {
            {"MANUFACTURER", &manufacturerValue}, {"BRAND", &brandValue},
            {"MODEL", &modelValue},               {"PRODUCT", &productValue},
            {"DEVICE", &deviceValue},             {"ID", &idValue},
            {"INCREMENTAL", &incrementalValue},   {"TYPE", &typeValue},
            {"TAGS", &tagsValue},                 {"RELEASE", &releaseValue},
            {"SECURITY_PATCH", &securityPatchValue},
        };
        for (const auto &[key, target] : stringFields) {
            const auto it = json.find(key);
            if (it != json.end() && it->is_string()) *target = it->get<std::string>();
        }

        if (pkgName == VENDING_PACKAGE) {
            json = nlohmann::json::object();
            return;
        }

        popFlag("spoofBuild", spoofBuild, "Spoofing Build Fields");
        popFlag("spoofProps", spoofProps, "Spoofing System Properties");
        popFlag("spoofProvider", spoofProvider, "Spoofing Keystore Provider");
        popFlag("spoofSignature", spoofSignature, "Spoofing ROM Signature");

        for (auto it = json.begin(); it != json.end();) {
            const std::string key = it.key();
            if (key.find_first_of("*.") == std::string::npos) {
                ++it;
                continue;
            }

            if (verboseLogs > 1) LOGD("Parsing %s", key.c_str());

            if (!it.value().is_string()) {
                LOGD("Error parsing %s!", key.c_str());
            } else if (it.value().get<std::string>().empty()) {
                LOGD("%s is empty, skipping", key.c_str());
            } else {
                if (verboseLogs > 0) LOGD("Adding '%s' to properties list", key.c_str());
                jsonProps[key] = it.value().get<std::string>();
            }
            it = json.erase(it);
        }
    }

    // --- spoofing entry points -----------------------------------------------

    void spoofSystem() {
        LOGD("Resume state: Continuing injection for %s", pkgName.c_str());

        if (spoofProps > 0) {
            if (installVirtualizer(jsonProps, json, true)) {
                LOGD("Property-area virtualization installed in %s", processName.c_str());
            } else {
                LOGD("Property-area virtualization failed in %s; falling back to property hook",
                     processName.c_str());
                doHook();
            }
        }

        if (spoofBuild + spoofProvider + spoofSignature +
            spoofVendingFinger + spoofVendingSdk + spoofPatch + spoofPixel > 0) {
            inject();
        }
    }

    void spoofPhotosApp() {
        if (spoofPhotos <= 0 || photosConfig.empty()) {
            LOGD("Photos spoofing disabled or sailfish.prop missing (spoofPhotos=%d)", spoofPhotos);
            return;
        }

        photosJson = parseConfig(photosConfig, true);
        if (photosJson.empty()) return;

        readPhotosJson();
        if (!photosJsonProps.empty() && !installVirtualizer(photosJsonProps, photosJson, false)) {
            jsonProps = photosJsonProps;
            LOGD("Photos property virtualizer unavailable; using Photos-only fallback (%d props)",
                 static_cast<int>(jsonProps.size()));
            doHook();
        }
        if (!photosJson.empty()) injectPhotos();
    }

    // --- JNI injection ---------------------------------------------------------

    // Must be called with a ScopedLocalFrame active; every local ref created here dies with it.
    jclass loadEntryClass(const char *className, const char *tag) {
        if (dexVector.empty()) {
            LOGD("JNI %s: module DEX is empty; skipping injection", tag);
            return nullptr;
        }

        LOGD("JNI %s: Creating module classloader", tag);

        jclass classLoaderClass = env->FindClass("java/lang/ClassLoader");
        if (checkJniException(env, "FindClass(ClassLoader)") || classLoaderClass == nullptr)
            return nullptr;

        jmethodID getSystemClassLoader = env->GetStaticMethodID(
                classLoaderClass, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
        if (checkJniException(env, "getSystemClassLoader lookup") || getSystemClassLoader == nullptr)
            return nullptr;

        jobject systemClassLoader = env->CallStaticObjectMethod(classLoaderClass, getSystemClassLoader);
        if (checkJniException(env, "getSystemClassLoader call") || systemClassLoader == nullptr)
            return nullptr;

        jclass dexClassLoaderClass = env->FindClass("dalvik/system/InMemoryDexClassLoader");
        if (checkJniException(env, "FindClass(InMemoryDexClassLoader)") || dexClassLoaderClass == nullptr)
            return nullptr;

        jmethodID dexClassLoaderInit = env->GetMethodID(
                dexClassLoaderClass, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
        if (checkJniException(env, "InMemoryDexClassLoader constructor lookup") ||
            dexClassLoaderInit == nullptr)
            return nullptr;

        jobject buffer = env->NewDirectByteBuffer(dexVector.data(), static_cast<jlong>(dexVector.size()));
        if (checkJniException(env, "NewDirectByteBuffer") || buffer == nullptr) return nullptr;

        jobject dexClassLoader = env->NewObject(dexClassLoaderClass, dexClassLoaderInit, buffer, systemClassLoader);
        if (checkJniException(env, "NewObject(InMemoryDexClassLoader)") || dexClassLoader == nullptr)
            return nullptr;

        LOGD("JNI %s: Loading module class", tag);

        jmethodID loadClass = env->GetMethodID(
                classLoaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        if (checkJniException(env, "ClassLoader.loadClass lookup") || loadClass == nullptr)
            return nullptr;

        jstring name = env->NewStringUTF(className);
        if (checkJniException(env, "NewStringUTF(className)") || name == nullptr) return nullptr;

        auto entryClass = static_cast<jclass>(env->CallObjectMethod(dexClassLoader, loadClass, name));
        if (checkJniException(env, "loadClass call") || entryClass == nullptr) {
            LOGD("JNI %s: unable to load '%s'; skipping injection", tag, className);
            return nullptr;
        }

        const JNINativeMethod methods[] = {
            {const_cast<char *>("setFieldNative"),
             const_cast<char *>("(Ljava/lang/Class;Ljava/lang/reflect/Field;Ljava/lang/String;Ljava/lang/Object;)V"),
             reinterpret_cast<void *>(setFieldNative)},
        };
        const jint result = env->RegisterNatives(entryClass, methods, 1);
        if (checkJniException(env, "RegisterNatives") || result != JNI_OK) {
            LOGD("JNI %s: native method registration failed; skipping injection", tag);
            return nullptr;
        }

        return entryClass;
    }

    bool sendJson(jclass entryClass, const nlohmann::json &payload, const char *tag) {
        LOGD("JNI %s: Sending JSON", tag);

        jmethodID receiveJson = env->GetStaticMethodID(entryClass, "receiveJson", "(Ljava/lang/String;)V");
        if (checkJniException(env, "receiveJson lookup") || receiveJson == nullptr) {
            LOGD("JNI %s: receiveJson lookup failed; skipping injection", tag);
            return false;
        }

        jstring javaJson = env->NewStringUTF(payload.dump().c_str());
        if (checkJniException(env, "JSON allocation") || javaJson == nullptr) return false;

        env->CallStaticVoidMethod(entryClass, receiveJson, javaJson);
        return !checkJniException(env, "receiveJson call");
    }

    void inject() {
        const bool vending = pkgName == VENDING_PACKAGE;
        const char *tag = vending ? "PS" : "DG";

        ScopedLocalFrame frame(env, 64);
        if (!frame.valid()) return;

        jclass entryClass = loadEntryClass(vending ? ENTRY_CLASS_VENDING : ENTRY_CLASS_DROIDGUARD, tag);
        if (entryClass == nullptr) return;

        if (vending) initVending(entryClass);
        else initDroidGuard(entryClass);
    }

    void initVending(jclass entryClass) {
        LOGD("JNI PS: Calling EntryPointVending.init");

        jmethodID init = env->GetStaticMethodID(
                entryClass, "init",
                "(IIILjava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;"
                "Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
                "Ljava/lang/String;Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;)V");
        if (checkJniException(env, "EntryPointVending.init lookup") || init == nullptr) {
            LOGD("JNI PS: EntryPointVending.init lookup failed; skipping injection");
            return;
        }

        const std::string *values[] = {
            &vendingFingerprintValue, &manufacturerValue, &brandValue, &modelValue,
            &productValue, &deviceValue, &idValue, &incrementalValue, &typeValue,
            &tagsValue, &releaseValue, &securityPatchValue,
        };
        jstring s[std::size(values)];
        bool allocated = true;
        for (size_t i = 0; i < std::size(values); ++i) {
            s[i] = env->NewStringUTF(values[i]->c_str());
            allocated = allocated && s[i] != nullptr;
        }
        if (checkJniException(env, "Play Store argument allocation") || !allocated) {
            LOGD("JNI PS: failed to allocate initialization arguments; skipping injection");
            return;
        }

        env->CallStaticVoidMethod(entryClass, init,
                                  verboseLogs, spoofVendingFinger, spoofVendingSdk, s[0], spoofPixel,
                                  s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9],
                                  spoofPatch, s[10], s[11]);
        checkJniException(env, "EntryPointVending.init call");
    }

    void initDroidGuard(jclass entryClass) {
        if (!sendJson(entryClass, json, "DG")) return;

        LOGD("JNI DG: Calling EntryPoint.init");

        jmethodID init = env->GetStaticMethodID(entryClass, "init", "(IIII)V");
        if (checkJniException(env, "EntryPoint.init lookup") || init == nullptr) return;

        env->CallStaticVoidMethod(entryClass, init, verboseLogs, spoofBuild, spoofProvider, spoofSignature);
        checkJniException(env, "EntryPoint.init call");
    }

    void injectPhotos() {
        ScopedLocalFrame frame(env, 48);
        if (!frame.valid()) return;

        jclass entryClass = loadEntryClass(ENTRY_CLASS_PHOTOS, "PHOTOS");
        if (entryClass == nullptr || !sendJson(entryClass, photosJson, "PHOTOS")) return;

        jmethodID init = env->GetStaticMethodID(entryClass, "init", "(III)V");
        if (checkJniException(env, "PHOTOS init lookup") || init == nullptr) return;

        env->CallStaticVoidMethod(entryClass, init, verboseLogs, 1, 1);
        checkJniException(env, "PHOTOS init call");
    }

    void injectApps() {
        ScopedLocalFrame frame(env, 48);
        if (!frame.valid()) return;

        jclass entryClass = loadEntryClass(ENTRY_CLASS_APPS, "APPS");
        if (entryClass == nullptr) return;

        LOGD("JNI APPS: Calling EntryPointApps.init");

        jmethodID init = env->GetStaticMethodID(entryClass, "init", "(ILjava/lang/String;)V");
        if (checkJniException(env, "APPS init lookup") || init == nullptr) return;

        jstring javaJson = env->NewStringUTF(appJson.dump().c_str());
        if (checkJniException(env, "APPS JSON allocation") || javaJson == nullptr) return;

        env->CallStaticVoidMethod(entryClass, init, verboseLogs, javaJson);
        checkJniException(env, "APPS init call");
    }
};

// ---------------------------------------------------------------------------
// Companion (runs as root, outside the app process)
// ---------------------------------------------------------------------------

// Profiles are cached for the life of the companion, but reloaded when the file changes.
class CachedFile {
public:
    bool load(const char *path, std::vector<char> &out, size_t maxSize) {
        std::lock_guard<std::mutex> lock(mutex_);

        struct stat st {};
        if (stat(path, &st) != 0) {
            LOGD("Companion: cannot stat %s; errno=%d (%s)", path, errno, strerror(errno));
            ready_ = false;
            return false;
        }

        if (!ready_ || st.st_mtim.tv_sec != mtimeSec_ || st.st_mtim.tv_nsec != mtimeNsec_ ||
            st.st_size != size_) {
            if (!readFile(path, data_, maxSize)) {
                LOGD("Companion: cannot read %s", path);
                ready_ = false;
                return false;
            }
            mtimeSec_ = st.st_mtim.tv_sec;
            mtimeNsec_ = st.st_mtim.tv_nsec;
            size_ = st.st_size;
            ready_ = true;
            LOGD("Companion: loaded %s (%d bytes)", path, static_cast<int>(data_.size()));
        }

        out = data_;
        return true;
    }

private:
    std::mutex mutex_;
    std::vector<char> data_;
    bool ready_ = false;
    time_t mtimeSec_ = 0;
    long mtimeNsec_ = 0;
    off_t size_ = 0;
};

class LoadOnceFile {
public:
    bool load(const char *path, std::vector<char> &out, size_t maxSize) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!ready_) {
            if (!readFile(path, data_, maxSize)) {
                LOGD("Companion: cannot read %s", path);
                return false;
            }
            ready_ = true;
            LOGD("Companion: loaded %s (%d bytes)", path, static_cast<int>(data_.size()));
        }

        out = data_;
        return true;
    }

private:
    std::mutex mutex_;
    std::vector<char> data_;
    bool ready_ = false;
};

static CachedFile appProfileCache;
static LoadOnceFile photosProfileCache;

static void companion(int fd) {
    std::vector<char> dexVector;
    std::vector<char> configVector;
    std::vector<char> appProfileVector;
    std::vector<char> photosConfigVector;

    if (!readFile(DEX_FILE_PATH, dexVector, MAX_DEX_PAYLOAD) || dexVector.empty()) {
        LOGD("Companion: failed to read a valid dex payload");
        dexVector.clear();
    }

    static constexpr const char *configPaths[] = {
        CUSTOM_PROP_FILE_PATH, CUSTOM_JSON_FILE_PATH, PROP_FILE_PATH, JSON_FILE_PATH,
    };
    const char *configSource = nullptr;
    for (const char *path : configPaths) {
        if (readFile(path, configVector, MAX_CONFIG_PAYLOAD)) {
            configSource = path;
            break;
        }
    }
    LOGD("Companion config source: %s; bytes=%d", configSource != nullptr ? configSource : "none",
         static_cast<int>(configVector.size()));

    if (!appProfileCache.load(APP_PROFILE_FILE_PATH, appProfileVector, MAX_APP_PROFILE_PAYLOAD))
        appProfileVector.clear();

    if (!photosProfileCache.load(PHOTOS_PROP_FILE_PATH, photosConfigVector, MAX_CONFIG_PAYLOAD))
        photosConfigVector.clear();

    const PayloadHeader header{
        static_cast<int64_t>(dexVector.size()),
        static_cast<int64_t>(configVector.size()),
        static_cast<int64_t>(appProfileVector.size()),
        static_cast<int64_t>(photosConfigVector.size()),
    };

    const bool sent = writeFully(fd, &header, sizeof(header)) &&
                      writeFully(fd, dexVector.data(), dexVector.size()) &&
                      writeFully(fd, configVector.data(), configVector.size()) &&
                      writeFully(fd, appProfileVector.data(), appProfileVector.size()) &&
                      writeFully(fd, photosConfigVector.data(), photosConfigVector.size());

    if (!sent) LOGD("Companion: failed to send complete payload to Zygisk process");
}

REGISTER_ZYGISK_MODULE(PlayIntegrityFix)

REGISTER_ZYGISK_COMPANION(companion)
