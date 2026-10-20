#include <android/log.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <sys/stat.h>

#include "zygisk.hpp"
#include "json/single_include/nlohmann/json.hpp"
#include "property_virtualizer.hpp"
#include "pif_config.hpp"

#if defined(__aarch64__) || defined(__arm__)
#include "shadowhook.h"
#include <dlfcn.h>
#endif

#include <map>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "PIF/Native", __VA_ARGS__)

#define DEX_FILE_PATH "/data/adb/modules/playintegrityfix/classes.dex"

#define PROP_FILE_PATH "/data/adb/modules/playintegrityfix/pif.prop"
#define CUSTOM_PROP_FILE_PATH "/data/adb/modules/playintegrityfix/custom.pif.prop"

#define JSON_FILE_PATH "/data/adb/modules/playintegrityfix/pif.json"
#define CUSTOM_JSON_FILE_PATH "/data/adb/modules/playintegrityfix/custom.pif.json"

#define APPS_FILE_PATH "/data/adb/modules/playintegrityfix/apps.txt"
#define APP_PROFILE_FILE_PATH "/data/adb/modules/playintegrityfix/app_profile.prop"

#define STOP_FILE "/data/adb/modules/playintegrityfix/stop"

#define VENDING_PACKAGE "com.android.vending"
#define DROIDGUARD_PACKAGE "com.google.android.gms.unstable"

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

static std::map<std::string, std::string> jsonProps;
static std::map<std::string, std::string> appJsonProps;
static nlohmann::json appJson = nlohmann::json::object();
static std::string appProfileData;

// File-descriptor I/O may complete partially or be interrupted by a signal.
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

static int parseIntOrKeep(const std::string &value, int current, const char *key) {
    if (value.empty()) {
        LOGD("Invalid integer for %s; keeping previous value %d", key, current);
        return current;
    }

    errno = 0;
    char *end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);

    if (errno == ERANGE || end == value.c_str() || end == nullptr ||
        *end != '\0' || parsed < INT_MIN || parsed > INT_MAX) {
        LOGD("Invalid integer for %s; keeping previous value %d", key, current);
        return current;
    }

    return static_cast<int>(parsed);
}

static bool readFileIntoVector(FILE *file, std::vector<char> &out, size_t maxSize) {
    if (file == nullptr || fseek(file, 0, SEEK_END) != 0) return false;
    const long length = ftell(file);
    if (length < 0 || static_cast<unsigned long long>(length) > maxSize ||
        fseek(file, 0, SEEK_SET) != 0) return false;

    out.resize(static_cast<size_t>(length));
    if (!out.empty() && fread(out.data(), 1, out.size(), file) != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

static constexpr size_t MAX_DEX_PAYLOAD = 64U * 1024U * 1024U;
static constexpr size_t MAX_CONFIG_PAYLOAD = 16U * 1024U * 1024U;
static constexpr size_t MAX_APP_PROFILE_PAYLOAD = 16U * 1024U * 1024U;

#if defined(__aarch64__) || defined(__arm__)
typedef void (*T_Callback)(void *, const char *, const char *, uint32_t);

// Android invokes the property callback synchronously. Keep the original callback and
// cookie in a stack context instead of a process-global map keyed by caller-owned cookies.
struct CallbackContext {
    T_Callback callback;
    void *originalCookie;
};

static void modify_callback(void *cookie, const char *name, const char *value, uint32_t serial) {
    auto *context = static_cast<CallbackContext *>(cookie);
    if (context == nullptr || context->callback == nullptr) return;

    const char *replacement = value;
    if (name != nullptr && value != nullptr) {
        const std::string_view prop(name);
        const auto exact = jsonProps.find(std::string(prop));
        if (exact != jsonProps.end()) {
            replacement = exact->second.c_str();
        } else {
            for (const auto &entry : jsonProps) {
                const std::string_view pattern(entry.first);
                if (!pattern.empty() && pattern.front() == '*' &&
                    prop.ends_with(pattern.substr(1))) {
                    replacement = entry.second.c_str();
                    break;
                }
            }
        }

        if (verboseLogs > 0 && replacement != nullptr && strcmp(value, replacement) != 0) {
            LOGD("[%s]: %s -> %s", name, value, replacement);
        } else if (verboseLogs > 99 && replacement != nullptr) {
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
    shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);
    void *sym_addr = dlsym(RTLD_DEFAULT, "__system_property_read_callback");
    if (sym_addr == nullptr) {
        LOGD("Couldn't find '__system_property_read_callback' via dlsym");
        return;
    }

    void *handle = shadowhook_hook_sym_addr(
            sym_addr,
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

static bool installPropertyVirtualizer(const nlohmann::json &source) {
    pif::Config config;
    config.debug = verboseLogs > 1;

    for (const auto &[key, value] : jsonProps) {
        config.propMap[key] = value;
    }

    for (const auto &item : source.items()) {
        if (item.value().is_string()) {
            config.propMap[item.key()] = item.value().get<std::string>();
        }
    }

    // Fingerprint expansion 
    const auto fingerprintIt = config.propMap.find("FINGERPRINT");
    if (fingerprintIt != config.propMap.end() && !fingerprintIt->second.empty()) {
        std::vector<std::string> parts;
        std::string current;
        for (char ch : fingerprintIt->second) {
            if (ch == '/' || ch == ':') {
                parts.push_back(current);
                current.clear();
            } else {
                current.push_back(ch);
            }
        }
        parts.push_back(current);

        static const char *fieldNames[] = {
            "BRAND", "PRODUCT", "DEVICE", "RELEASE",
            "ID", "INCREMENTAL", "TYPE", "TAGS"
        };
        for (size_t i = 0; i < sizeof(fieldNames) / sizeof(fieldNames[0]); ++i) {
            if (config.propMap.find(fieldNames[i]) == config.propMap.end() && i < parts.size()) {
                config.propMap[fieldNames[i]] = parts[i];
            }
        }
    }

    auto getConfigValue = [](const nlohmann::json &source, const char *key) -> std::string {
        if (source.contains(key) && source[key].is_string()) {
            return source[key].get<std::string>();
        }
        return {};
    };

    config.buildId = getConfigValue(source, "ID");
    config.securityPatch = getConfigValue(source, "SECURITY_PATCH");
    const std::string initialSdk = getConfigValue(source, "DEVICE_INITIAL_SDK_INT");
    if (!initialSdk.empty()) config.deviceInitialSdkInt = initialSdk;

    const bool installed = pif::property_virtualizer::install(config);
    if (!installed) {
        LOGD("Property-area virtualization failed; attempting callback-hook fallback");
    }
    return installed;
}

static bool isPackageInAppsList(const std::string &pkgName) {
    FILE *appsFile = fopen(APPS_FILE_PATH, "r");

    if (!appsFile) return false;

    bool found = false;
    char line[256];

    while (fgets(line, sizeof(line), appsFile)) {
        size_t len = strlen(line);

        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        len = strlen(line);

        if (len > 0 && line[len - 1] == '\r')
            line[len - 1] = '\0';

        if (strlen(line) > 0 && pkgName == line) {
            found = true;
            break;
        }
    }

    fclose(appsFile);
    return found;
}

static bool isPaused() {
    struct stat st {};
    return stat(STOP_FILE, &st) == 0;
}

static bool loadAppProfile() {
    appJson.clear();
    appJsonProps.clear();

    if (appProfileData.empty()) {
        LOGD("App profile payload is empty");
        return false;
    }

    nlohmann::json profileBuild = nlohmann::json::object();
    size_t begin = 0;

    while (begin < appProfileData.size()) {
        size_t end = appProfileData.find('\n', begin);
        if (end == std::string::npos) end = appProfileData.size();

        std::string entry = appProfileData.substr(begin, end - begin);
        begin = end + 1;

        if (!entry.empty() && entry.back() == '\r') entry.pop_back();

        size_t first = entry.find_first_not_of(" \t");
        if (first == std::string::npos || entry[first] == '#') continue;

        size_t delimiter = entry.find('=', first);
        if (delimiter == std::string::npos) continue;

        size_t nameEnd = delimiter;
        while (nameEnd > first && (entry[nameEnd - 1] == ' ' || entry[nameEnd - 1] == '\t')) nameEnd--;

        size_t valueStart = delimiter + 1;
        while (valueStart < entry.size() && (entry[valueStart] == ' ' || entry[valueStart] == '\t')) valueStart++;

        std::string name = entry.substr(first, nameEnd - first);
        std::string value = entry.substr(valueStart);

        size_t comment = value.find('#');
        if (comment != std::string::npos) value.resize(comment);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();

        if (name.empty() || value.empty()) continue;

        // Ignore empty values in app_profile.prop
        appJsonProps[name] = value;

        const char *buildField = nullptr;
        if (name == "ro.product.manufacturer") buildField = "MANUFACTURER";
        else if (name == "ro.product.brand") buildField = "BRAND";
        else if (name == "ro.product.model") buildField = "MODEL";
        else if (name == "ro.product.name") buildField = "PRODUCT";
        else if (name == "ro.product.device") buildField = "DEVICE";
        else if (name == "ro.product.board") buildField = "BOARD";
        else if (name == "ro.build.fingerprint") buildField = "FINGERPRINT";
        else if (name == "ro.build.version.release") buildField = "RELEASE";
        else if (name == "ro.build.version.sdk") buildField = "SDK";
        else if (name == "ro.build.id") buildField = "ID";
        else if (name == "ro.build.version.incremental") buildField = "INCREMENTAL";
        else if (name == "ro.build.type") buildField = "TYPE";
        else if (name == "ro.build.tags") buildField = "TAGS";
        else if (name == "ro.build.version.security_patch") buildField = "SECURITY_PATCH";
        else if (name == "ro.build.version.codename") buildField = "CODENAME";
        else if (name == "ro.build.version.all_codenames") buildField = "ALL_CODENAMES";
        else if (name == "ro.build.version.base_os") buildField = "BASE_OS";
        else if (name == "ro.build.display.id") buildField = "DISPLAY";
        else if (name == "ro.build.user") buildField = "USER";
        else if (name == "ro.build.host") buildField = "HOST";
        else if (name == "ro.bootloader") buildField = "BOOTLOADER";
        else if (name == "ro.hardware") buildField = "HARDWARE";
        else if (name == "ro.soc.manufacturer") buildField = "SOC_MANUFACTURER";
        else if (name == "ro.soc.model") buildField = "SOC_MODEL";
        else if (name == "ro.soc.manufacturer") buildField = "SOC_MANUFACTURER";

        if (buildField) profileBuild[buildField] = value;
    }

    appJson = profileBuild;
    auto timezoneIt = appJsonProps.find("persist.sys.timezone");
    if (timezoneIt != appJsonProps.end())
        appJson["persist.sys.timezone"] = timezoneIt->second;

    // Report the hardware/version identity
    auto logProfileValue = [&](const char *name) {
        auto it = appJsonProps.find(name);
        if (it != appJsonProps.end())
            LOGD("Profile %s=%s", name, it->second.c_str());
    };
    logProfileValue("ro.build.version.release");
    logProfileValue("ro.build.version.sdk");
    logProfileValue("ro.hardware");
    logProfileValue("ro.board.platform");
    logProfileValue("ro.soc.manufacturer");
    logProfileValue("ro.soc.model");

    LOGD("Loaded app profile: %d properties, %d Build fields", static_cast<int>(appJsonProps.size()), static_cast<int>(profileBuild.size()));
    return !appJsonProps.empty();
}

static void setFieldNative(
        JNIEnv *env,
        jclass /* clazz_EntryPoint */,
        jclass targetClass,
        jobject fieldObj,
        jstring typeObj,
        jobject valueObj) {

    if (!targetClass || !fieldObj || !typeObj) return;

    jfieldID fieldID = env->FromReflectedField(fieldObj);

    if (!fieldID) return;

    const char *typeName = env->GetStringUTFChars(typeObj, nullptr);

    if (strcmp(typeName, "java.lang.String") == 0) {
        env->SetStaticObjectField(targetClass, fieldID, valueObj);
    } else if (strcmp(typeName, "int") == 0) {
        jclass intClass = env->FindClass("java/lang/Integer");
        jmethodID intValue = env->GetMethodID(intClass, "intValue", "()I");
        jint val = env->CallIntMethod(valueObj, intValue);
        env->SetStaticIntField(targetClass, fieldID, val);
    } else if (strcmp(typeName, "long") == 0) {
        jclass longClass = env->FindClass("java/lang/Long");
        jmethodID longValue = env->GetMethodID(longClass, "longValue", "()J");
        jlong val = env->CallLongMethod(valueObj, longValue);
        env->SetStaticLongField(targetClass, fieldID, val);
    } else if (strcmp(typeName, "boolean") == 0) {
        jclass boolClass = env->FindClass("java/lang/Boolean");
        jmethodID booleanValue = env->GetMethodID(boolClass, "booleanValue", "()Z");
        jboolean val = env->CallBooleanMethod(valueObj, booleanValue);
        env->SetStaticBooleanField(targetClass, fieldID, val);
    } else if (strcmp(typeName, "[Ljava.lang.String;") == 0) {
        env->SetStaticObjectField(targetClass, fieldID, valueObj);
    }

    env->ReleaseStringUTFChars(typeObj, typeName);
}

class PlayIntegrityFix : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        bool isGms = false;
        bool isDroidGuardOrVending = false;

        auto rawProcess = env->GetStringUTFChars(args->nice_name, nullptr);
        auto rawDir = env->GetStringUTFChars(args->app_data_dir, nullptr);

        if (rawDir == nullptr) {
            if (rawProcess != nullptr)
                env->ReleaseStringUTFChars(args->nice_name, rawProcess);

            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        pkgName = rawProcess;
        std::string_view dir(rawDir);

        isGms =
                dir.ends_with("/com.google.android.gms") ||
                dir.ends_with("/com.android.vending");

        isDroidGuardOrVending =
                pkgName == DROIDGUARD_PACKAGE ||
                pkgName == VENDING_PACKAGE;

        env->ReleaseStringUTFChars(args->nice_name, rawProcess);
        env->ReleaseStringUTFChars(args->app_data_dir, rawDir);

        // Determine per-app spoofing target before deciding whether to unload the module
        isTargetApp = isPackageInAppsList(pkgName);

        if (!isGms && !isTargetApp) {
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        api->setOption(zygisk::FORCE_DENYLIST_UNMOUNT);

        if (!isDroidGuardOrVending && !isTargetApp) {
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        if (isDroidGuardOrVending && isPaused()) {
            LOGD("ZYGISK: Injection is paused by user, skipping Integrity-Box spoofing for %s", pkgName.c_str());
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        std::vector<char> configVector;
        long dexSize = 0;
        long configSize = 0;
        long appProfileSize = 0;

        int fd = api->connectCompanion();

        if (fd < 0) {
            LOGD("Couldn't connect to companion");
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        if (!readFully(fd, &dexSize, sizeof(long)) ||
            !readFully(fd, &configSize, sizeof(long)) ||
            !readFully(fd, &appProfileSize, sizeof(long))) {
            close(fd);
            LOGD("Couldn't read companion sizes");
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        if (dexSize < 1 || static_cast<unsigned long long>(dexSize) > MAX_DEX_PAYLOAD ||
            configSize < 1 || static_cast<unsigned long long>(configSize) > MAX_CONFIG_PAYLOAD ||
            appProfileSize < 0 || static_cast<unsigned long long>(appProfileSize) > MAX_APP_PROFILE_PAYLOAD) {
            close(fd);
            LOGD("Companion returned invalid payload sizes (dex=%ld, config=%ld, profile=%ld)",
                 dexSize, configSize, appProfileSize);
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }


        LOGD("Read from file descriptor for 'dex' -> %ld bytes", dexSize);
        LOGD("Read from file descriptor for 'config' -> %ld bytes", configSize);
        LOGD("Read from file descriptor for 'app profile' -> %ld bytes", appProfileSize);

        dexVector.resize(dexSize);

        if (!readFully(fd, dexVector.data(), static_cast<size_t>(dexSize))) {
            close(fd);
            LOGD("Couldn't read dex payload");
            dexVector.clear();
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        configVector.resize(configSize);

        if (!readFully(fd, configVector.data(), static_cast<size_t>(configSize))) {
            close(fd);
            LOGD("Couldn't read config payload");
            dexVector.clear();
            configVector.clear();
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        if (appProfileSize > 0) {
            appProfileData.resize(appProfileSize);
            if (!readFully(fd, appProfileData.data(), static_cast<size_t>(appProfileSize))) {
                close(fd);
                LOGD("Couldn't read app profile payload");
                dexVector.clear();
                configVector.clear();
                appProfileData.clear();
                api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
                return;
            }
        } else {
            appProfileData.clear();
        }

        close(fd);

        std::string configString(configVector.cbegin(), configVector.cend());

        if (!nlohmann::json::accept(configString, true)) {
            LOGD("Converting config from prop format to JSON format");

            configString.erase(
                    std::remove(configString.begin(), configString.end(), '\r'),
                    configString.end()
            );

            std::string jsonString = "{";

            char propDelimiter = '=';
            char commentDelimiter = '#';

            size_t beginPos = 0;
            size_t endPos = 0;

            while ((endPos = configString.find('\n', beginPos)) != std::string::npos) {
                std::string line = configString.substr(beginPos, endPos - beginPos);
                beginPos = endPos + 1;

                if (line.empty() || line[0] == '#')
                    continue;

                std::string name;
                std::string value;

                size_t propDelimiterPos = line.find(propDelimiter);

                if (propDelimiterPos != std::string::npos) {
                    name = line.substr(0, propDelimiterPos);
                    value = line.substr(propDelimiterPos + 1);
                } else {
                    LOGD("Invalid prop entry, skipping");
                    continue;
                }

                size_t commentDelimiterPos = value.find(commentDelimiter);

                if (commentDelimiterPos != std::string::npos) {
                    value = value.substr(0, commentDelimiterPos);

                    size_t lastPos = value.find_last_not_of(" ");

                    if (lastPos != std::string::npos)
                        value.resize(lastPos + 1);
                }

                jsonString += "\n\"" + name + "\": \"" + value + "\",";
            }

            if (!jsonString.empty() && jsonString.back() == ',')
                jsonString.pop_back();

            jsonString += "\n}\n";

            configString = jsonString;
            jsonString.clear();
        }

        json = nlohmann::json::parse(configString, nullptr, false, true);

        configVector.clear();
        configString.clear();
    }

    static bool hasPairIpCoreLoaded() {
        FILE *maps = fopen("/proc/self/maps", "r");
        if (!maps) return false;

        char line[1024];
        bool found = false;
        while (fgets(line, sizeof(line), maps)) {
            if (strstr(line, "libpairipcore.so") != nullptr) {
                found = true;
                break;
            }
        }
        fclose(maps);
        return found;
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        if (dexVector.empty() || json.empty())
            return;

        readJson();

        if (pkgName == VENDING_PACKAGE)
            spoofBuild = spoofProps = spoofProvider = spoofSignature = 0;
        else
            spoofVendingFinger = spoofVendingSdk = spoofPatch = spoofPixel = 0;

        bool isDroidGuardOrVending =
                pkgName == DROIDGUARD_PACKAGE ||
                pkgName == VENDING_PACKAGE;

        if (isTargetApp && spoofApps > 0) {
            // Do not inject into a process already protected by PairIP
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

                dexVector.clear();
                json.clear();
                return;
            }

            LOGD("Resume state: Continuing injection for %s", pkgName.c_str());

            if (spoofProps > 0 && !installPropertyVirtualizer(json))
                doHook();

            if (spoofBuild +
                spoofProvider +
                spoofSignature +
                spoofVendingFinger +
                spoofVendingSdk +
                spoofPatch +
                spoofPixel > 0) {
                inject();
            }
        }

        dexVector.clear();
        json.clear();
        appJson.clear();
        appJsonProps.clear();
        appProfileData.clear();
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs *args) override {
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

private:
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;

    std::vector<char> dexVector;
    nlohmann::json json;
    std::string pkgName;
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

    void readJson() {
        LOGD("JSON contains %d keys!", static_cast<int>(json.size()));

        if (json.contains("verboseLogs")) {
            if (!json["verboseLogs"].is_null() &&
                json["verboseLogs"].is_string() &&
                json["verboseLogs"] != "") {

                verboseLogs = parseIntOrKeep(json["verboseLogs"].get<std::string>(), verboseLogs, "verboseLogs");

                if (verboseLogs > 0)
                    LOGD("Verbose logging (level %d) enabled!", verboseLogs);
            } else {
                LOGD("Error parsing verboseLogs!");
            }

            json.erase("verboseLogs");
        }

        if (json.contains("spoofVendingSdk")) {
            if (!json["spoofVendingSdk"].is_null() &&
                json["spoofVendingSdk"].is_string() &&
                json["spoofVendingSdk"] != "") {

                spoofVendingSdk = parseIntOrKeep(json["spoofVendingSdk"].get<std::string>(), spoofVendingSdk, "spoofVendingSdk");

                if (verboseLogs > 0)
                    LOGD(
                            "Spoofing SDK Level in Play Store %s!",
                            (spoofVendingSdk > 0) ? "enabled" : "disabled"
                    );
            } else {
                LOGD("Error parsing spoofVendingSdk!");
            }

            json.erase("spoofVendingSdk");
        }

        if (json.contains("spoofPatch")) {
            if (!json["spoofPatch"].is_null() &&
                json["spoofPatch"].is_string() &&
                json["spoofPatch"] != "") {

                spoofPatch = parseIntOrKeep(json["spoofPatch"].get<std::string>(), spoofPatch, "spoofPatch");

                if (verboseLogs > 0)
                    LOGD("Spoofing Android release and security patch %s!",
                         (spoofPatch > 0) ? "enabled" : "disabled");
            } else {
                LOGD("Error parsing spoofPatch!");
            }

            json.erase("spoofPatch");
        }

        if (json.contains("spoofVendingFinger")) {
            if (!json["spoofVendingFinger"].is_null() &&
                json["spoofVendingFinger"].is_string() &&
                json["spoofVendingFinger"] != "") {

                if (json["spoofVendingFinger"].get<std::string>().find_first_not_of("01") !=
                    std::string::npos) {

                    spoofVendingFinger = 1;
                    vendingFingerprintValue =
                            json["spoofVendingFinger"].get<std::string>();

                } else if (json.contains("FINGERPRINT") &&
                           !json["FINGERPRINT"].is_null() &&
                           json["FINGERPRINT"].is_string() &&
                           json["FINGERPRINT"] != "") {

                    spoofVendingFinger =
                            parseIntOrKeep(json["spoofVendingFinger"].get<std::string>(), spoofVendingFinger, "spoofVendingFinger");

                    vendingFingerprintValue =
                            json["FINGERPRINT"].get<std::string>();
                } else {
                    LOGD("Error parsing spoofVendingFinger or FINGERPRINT field!");
                }

                if (verboseLogs > 0)
                    LOGD(
                            "Spoofing Fingerprint in Play Store %s!",
                            (spoofVendingFinger > 0) ? "enabled" : "disabled"
                    );
            } else {
                LOGD("Error parsing spoofVendingFinger!");
            }

            json.erase("spoofVendingFinger");
        }

        if (json.contains("spoofPixel")) {
            if (!json["spoofPixel"].is_null() &&
                json["spoofPixel"].is_string() &&
                json["spoofPixel"] != "") {

                spoofPixel = parseIntOrKeep(json["spoofPixel"].get<std::string>(), spoofPixel, "spoofPixel");

                if (verboseLogs > 0)
                    LOGD(
                            "Spoofing Pixel Fields in Play Store %s!",
                            (spoofPixel > 0) ? "enabled" : "disabled"
                    );
            } else {
                LOGD("Error parsing spoofPixel!");
            }

            json.erase("spoofPixel");
        }

        if (json.contains("spoofApps")) {
            if (!json["spoofApps"].is_null() &&
                json["spoofApps"].is_string() &&
                json["spoofApps"] != "") {

                spoofApps = parseIntOrKeep(json["spoofApps"].get<std::string>(), spoofApps, "spoofApps");

                if (verboseLogs > 0)
                    LOGD(
                            "Per-app spoofing %s!",
                            (spoofApps > 0) ? "enabled" : "disabled"
                    );
            } else {
                LOGD("Error parsing spoofApps!");
            }

            json.erase("spoofApps");
        }

        auto readStringField = [this](const char *key, std::string &target) {
            if (json.contains(key) && json[key].is_string()) {
                target = json[key].get<std::string>();
            }
        };

        readStringField("MANUFACTURER", manufacturerValue);
        readStringField("BRAND", brandValue);
        readStringField("MODEL", modelValue);
        readStringField("PRODUCT", productValue);
        readStringField("DEVICE", deviceValue);
        readStringField("ID", idValue);
        readStringField("INCREMENTAL", incrementalValue);
        readStringField("TYPE", typeValue);
        readStringField("TAGS", tagsValue);
        readStringField("RELEASE", releaseValue);
        readStringField("SECURITY_PATCH", securityPatchValue);

        if (pkgName == VENDING_PACKAGE) {
            json.clear();
            return;
        }

        if (json.contains("spoofBuild")) {
            if (!json["spoofBuild"].is_null() &&
                json["spoofBuild"].is_string() &&
                json["spoofBuild"] != "") {

                spoofBuild = parseIntOrKeep(json["spoofBuild"].get<std::string>(), spoofBuild, "spoofBuild");

                if (verboseLogs > 0)
                    LOGD(
                            "Spoofing Build Fields %s!",
                            (spoofBuild > 0) ? "enabled" : "disabled"
                    );
            } else {
                LOGD("Error parsing spoofBuild!");
            }

            json.erase("spoofBuild");
        }

        if (json.contains("spoofProps")) {
            if (!json["spoofProps"].is_null() &&
                json["spoofProps"].is_string() &&
                json["spoofProps"] != "") {

                spoofProps = parseIntOrKeep(json["spoofProps"].get<std::string>(), spoofProps, "spoofProps");

                if (verboseLogs > 0)
                    LOGD(
                            "Spoofing System Properties %s!",
                            (spoofProps > 0) ? "enabled" : "disabled"
                    );
            } else {
                LOGD("Error parsing spoofProps!");
            }

            json.erase("spoofProps");
        }

        if (json.contains("spoofProvider")) {
            if (!json["spoofProvider"].is_null() &&
                json["spoofProvider"].is_string() &&
                json["spoofProvider"] != "") {

                spoofProvider = parseIntOrKeep(json["spoofProvider"].get<std::string>(), spoofProvider, "spoofProvider");

                if (verboseLogs > 0)
                    LOGD(
                            "Spoofing Keystore Provider %s!",
                            (spoofProvider > 0) ? "enabled" : "disabled"
                    );
            } else {
                LOGD("Error parsing spoofProvider!");
            }

            json.erase("spoofProvider");
        }

        if (json.contains("spoofSignature")) {
            if (!json["spoofSignature"].is_null() &&
                json["spoofSignature"].is_string() &&
                json["spoofSignature"] != "") {

                spoofSignature = parseIntOrKeep(json["spoofSignature"].get<std::string>(), spoofSignature, "spoofSignature");

                if (verboseLogs > 0)
                    LOGD(
                            "Spoofing ROM Signature %s!",
                            (spoofSignature > 0) ? "enabled" : "disabled"
                    );
            } else {
                LOGD("Error parsing spoofSignature!");
            }

            json.erase("spoofSignature");
        }

        std::vector<std::string> eraseKeys;

        for (auto &jsonList : json.items()) {
            if (verboseLogs > 1)
                LOGD("Parsing %s", jsonList.key().c_str());

            if (jsonList.key().find_first_of("*.") != std::string::npos) {
                if (!jsonList.value().is_null() &&
                    jsonList.value().is_string()) {

                    if (jsonList.value() == "") {
                        LOGD(
                                "%s is empty, skipping",
                                jsonList.key().c_str()
                        );
                    } else {
                        if (verboseLogs > 0)
                            LOGD(
                                    "Adding '%s' to properties list",
                                    jsonList.key().c_str()
                            );

                        jsonProps[jsonList.key()] = jsonList.value();
                    }
                } else {
                    LOGD(
                            "Error parsing %s!",
                            jsonList.key().c_str()
                    );
                }

                eraseKeys.push_back(jsonList.key());
            }
        }

        for (auto key : eraseKeys) {
            if (json.contains(key))
                json.erase(key);
        }
    }

    void inject() {
        const char *niceName =
                pkgName == VENDING_PACKAGE ? "PS" : "DG";

        LOGD("JNI %s: Getting system classloader", niceName);

        auto clClass =
                env->FindClass("java/lang/ClassLoader");

        auto getSystemClassLoader =
                env->GetStaticMethodID(
                        clClass,
                        "getSystemClassLoader",
                        "()Ljava/lang/ClassLoader;"
                );

        auto systemClassLoader =
                env->CallStaticObjectMethod(
                        clClass,
                        getSystemClassLoader
                );

        LOGD("JNI %s: Creating module classloader", niceName);

        auto dexClClass =
                env->FindClass("dalvik/system/InMemoryDexClassLoader");

        auto dexClInit =
                env->GetMethodID(
                        dexClClass,
                        "<init>",
                        "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V"
                );

        auto buffer =
                env->NewDirectByteBuffer(
                        dexVector.data(),
                        static_cast<jlong>(dexVector.size())
                );

        auto dexCl =
                env->NewObject(
                        dexClClass,
                        dexClInit,
                        buffer,
                        systemClassLoader
                );

        LOGD("JNI %s: Loading module class", niceName);

        auto loadClass =
                env->GetMethodID(
                        clClass,
                        "loadClass",
                        "(Ljava/lang/String;)Ljava/lang/Class;"
                );

        const char *className =
                pkgName == VENDING_PACKAGE
                ? "es.chiteroman.playintegrityfix.EntryPointVending"
                : "es.chiteroman.playintegrityfix.EntryPoint";

        auto entryClassName =
                env->NewStringUTF(className);

        auto entryClassObj =
                env->CallObjectMethod(
                        dexCl,
                        loadClass,
                        entryClassName
                );

        if (env->ExceptionCheck()) {
            LOGD(
                    "JNI %s: Exception while loading class '%s', clearing and aborting injection",
                    niceName,
                    className
            );

            env->ExceptionClear();

            env->DeleteLocalRef(clClass);
            env->DeleteLocalRef(systemClassLoader);
            env->DeleteLocalRef(dexClClass);
            env->DeleteLocalRef(buffer);
            env->DeleteLocalRef(dexCl);
            env->DeleteLocalRef(entryClassName);

            return;
        }

        if (entryClassObj == nullptr) {
            LOGD(
                    "JNI %s: loadClass('%s') returned null, aborting injection",
                    niceName,
                    className
            );

            env->DeleteLocalRef(clClass);
            env->DeleteLocalRef(systemClassLoader);
            env->DeleteLocalRef(dexClClass);
            env->DeleteLocalRef(buffer);
            env->DeleteLocalRef(dexCl);
            env->DeleteLocalRef(entryClassName);

            return;
        }

        auto entryClass =
                (jclass) entryClassObj;

        JNINativeMethod methods[] = {
            {
                "setFieldNative",
                "(Ljava/lang/Class;Ljava/lang/reflect/Field;Ljava/lang/String;Ljava/lang/Object;)V",
                (void *) setFieldNative
            }
        };

        env->RegisterNatives(
                entryClass,
                methods,
                1
        );

        if (pkgName == VENDING_PACKAGE) {
            LOGD(
                    "JNI %s: Calling EntryPointVending.init",
                    niceName
            );

            auto entryInit =
                    env->GetStaticMethodID(
                            entryClass,
                            "init",
                            "(IIILjava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;)V"
                    );

            if (entryInit == nullptr || env->ExceptionCheck()) {
                LOGD("JNI %s: EntryPointVending.init method lookup failed; skipping injection", niceName);
                if (env->ExceptionCheck()) env->ExceptionClear();
            } else {
            auto javaStr =
                    env->NewStringUTF(
                            vendingFingerprintValue.c_str()
                    );

            auto manufacturerStr =
                    env->NewStringUTF(manufacturerValue.c_str());

            auto brandStr =
                    env->NewStringUTF(brandValue.c_str());

            auto modelStr =
                    env->NewStringUTF(modelValue.c_str());

            auto productStr =
                    env->NewStringUTF(productValue.c_str());

            auto deviceStr =
                    env->NewStringUTF(deviceValue.c_str());

            auto idStr =
                    env->NewStringUTF(idValue.c_str());

            auto incrementalStr =
                    env->NewStringUTF(incrementalValue.c_str());

            auto typeStr =
                    env->NewStringUTF(typeValue.c_str());

            auto tagsStr =
                    env->NewStringUTF(tagsValue.c_str());

            auto releaseStr =
                    env->NewStringUTF(releaseValue.c_str());

            auto securityPatchStr =
                    env->NewStringUTF(securityPatchValue.c_str());

            env->CallStaticVoidMethod(
                    entryClass,
                    entryInit,
                    verboseLogs,
                    spoofVendingFinger,
                    spoofVendingSdk,
                    javaStr,
                    spoofPixel,
                    manufacturerStr,
                    brandStr,
                    modelStr,
                    productStr,
                    deviceStr,
                    idStr,
                    incrementalStr,
                    typeStr,
                    tagsStr,
                    spoofPatch,
                    releaseStr,
                    securityPatchStr
            );

            env->DeleteLocalRef(javaStr);
            env->DeleteLocalRef(manufacturerStr);
            env->DeleteLocalRef(brandStr);
            env->DeleteLocalRef(modelStr);
            env->DeleteLocalRef(productStr);
            env->DeleteLocalRef(deviceStr);
            env->DeleteLocalRef(idStr);
            env->DeleteLocalRef(incrementalStr);
            env->DeleteLocalRef(typeStr);
            env->DeleteLocalRef(tagsStr);
            env->DeleteLocalRef(releaseStr);
            env->DeleteLocalRef(securityPatchStr);
            }
        } else {
            LOGD(
                    "JNI %s: Sending JSON",
                    niceName
            );

            auto receiveJson =
                    env->GetStaticMethodID(
                            entryClass,
                            "receiveJson",
                            "(Ljava/lang/String;)V"
                    );

            auto javaStr =
                    env->NewStringUTF(
                            json.dump().c_str()
                    );

            env->CallStaticVoidMethod(
                    entryClass,
                    receiveJson,
                    javaStr
            );

            LOGD(
                    "JNI %s: Calling EntryPoint.init",
                    niceName
            );

            auto entryInit =
                    env->GetStaticMethodID(
                            entryClass,
                            "init",
                            "(IIII)V"
                    );

            env->CallStaticVoidMethod(
                    entryClass,
                    entryInit,
                    verboseLogs,
                    spoofBuild,
                    spoofProvider,
                    spoofSignature
            );

            env->DeleteLocalRef(javaStr);
        }

        env->DeleteLocalRef(clClass);
        env->DeleteLocalRef(systemClassLoader);
        env->DeleteLocalRef(dexClClass);
        env->DeleteLocalRef(buffer);
        env->DeleteLocalRef(dexCl);
        env->DeleteLocalRef(entryClassName);
        env->DeleteLocalRef(entryClassObj);
    }

    void injectApps() {
        LOGD("JNI APPS: Getting system classloader");

        auto clClass =
                env->FindClass("java/lang/ClassLoader");

        auto getSystemClassLoader =
                env->GetStaticMethodID(
                        clClass,
                        "getSystemClassLoader",
                        "()Ljava/lang/ClassLoader;"
                );

        auto systemClassLoader =
                env->CallStaticObjectMethod(
                        clClass,
                        getSystemClassLoader
                );

        LOGD("JNI APPS: Creating module classloader");

        auto dexClClass =
                env->FindClass("dalvik/system/InMemoryDexClassLoader");

        auto dexClInit =
                env->GetMethodID(
                        dexClClass,
                        "<init>",
                        "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V"
                );

        auto buffer =
                env->NewDirectByteBuffer(
                        dexVector.data(),
                        static_cast<jlong>(dexVector.size())
                );

        auto dexCl =
                env->NewObject(
                        dexClClass,
                        dexClInit,
                        buffer,
                        systemClassLoader
                );

        LOGD("JNI APPS: Loading module class");

        auto loadClass =
                env->GetMethodID(
                        clClass,
                        "loadClass",
                        "(Ljava/lang/String;)Ljava/lang/Class;"
                );

        auto entryClassName =
                env->NewStringUTF(
                        "es.chiteroman.playintegrityfix.EntryPointApps"
                );

        auto entryClassObj =
                env->CallObjectMethod(
                        dexCl,
                        loadClass,
                        entryClassName
                );

        if (env->ExceptionCheck()) {
            LOGD(
                    "JNI APPS: Exception while loading EntryPointApps, clearing and aborting injection"
            );

            env->ExceptionClear();

            env->DeleteLocalRef(clClass);
            env->DeleteLocalRef(systemClassLoader);
            env->DeleteLocalRef(dexClClass);
            env->DeleteLocalRef(buffer);
            env->DeleteLocalRef(dexCl);
            env->DeleteLocalRef(entryClassName);

            return;
        }

        if (entryClassObj == nullptr) {
            LOGD(
                    "JNI APPS: loadClass('EntryPointApps') returned null, aborting injection"
            );

            env->DeleteLocalRef(clClass);
            env->DeleteLocalRef(systemClassLoader);
            env->DeleteLocalRef(dexClClass);
            env->DeleteLocalRef(buffer);
            env->DeleteLocalRef(dexCl);
            env->DeleteLocalRef(entryClassName);

            return;
        }

        auto entryClass =
                (jclass) entryClassObj;

        JNINativeMethod methods[] = {
            {
                "setFieldNative",
                "(Ljava/lang/Class;Ljava/lang/reflect/Field;Ljava/lang/String;Ljava/lang/Object;)V",
                (void *) setFieldNative
            }
        };

        env->RegisterNatives(
                entryClass,
                methods,
                1
        );

        LOGD("JNI APPS: Calling EntryPointApps.init");

        auto entryInit =
                env->GetStaticMethodID(
                        entryClass,
                        "init",
                        "(ILjava/lang/String;)V"
                );

        auto javaStr =
                env->NewStringUTF(
                        appJson.dump().c_str()
                );

        env->CallStaticVoidMethod(
                entryClass,
                entryInit,
                verboseLogs,
                javaStr
        );

        env->DeleteLocalRef(javaStr);

        env->DeleteLocalRef(clClass);
        env->DeleteLocalRef(systemClassLoader);
        env->DeleteLocalRef(dexClClass);
        env->DeleteLocalRef(buffer);
        env->DeleteLocalRef(dexCl);
        env->DeleteLocalRef(entryClassName);
        env->DeleteLocalRef(entryClassObj);
    }
};

static void companion(int fd) {
    std::vector<char> dexVector;
    std::vector<char> configVector;
    std::vector<char> appProfileVector;

    auto loadPath = [](const char *path, std::vector<char> &buffer, size_t maxSize) -> bool {
        FILE *file = fopen(path, "rb");
        if (file == nullptr) return false;
        const bool ok = readFileIntoVector(file, buffer, maxSize);
        fclose(file);
        if (!ok) buffer.clear();
        return ok;
    };

    if (!loadPath(DEX_FILE_PATH, dexVector, MAX_DEX_PAYLOAD) || dexVector.empty()) {
        LOGD("Companion: failed to read a valid dex payload");
        dexVector.clear();
    }

    bool configLoaded = loadPath(CUSTOM_PROP_FILE_PATH, configVector, MAX_CONFIG_PAYLOAD);
    if (!configLoaded) configLoaded = loadPath(CUSTOM_JSON_FILE_PATH, configVector, MAX_CONFIG_PAYLOAD);
    if (!configLoaded) configLoaded = loadPath(PROP_FILE_PATH, configVector, MAX_CONFIG_PAYLOAD);
    if (!configLoaded) configLoaded = loadPath(JSON_FILE_PATH, configVector, MAX_CONFIG_PAYLOAD);

    if (!loadPath(APP_PROFILE_FILE_PATH, appProfileVector, MAX_APP_PROFILE_PAYLOAD)) {
        appProfileVector.clear();
    }

    const long dexSize = static_cast<long>(dexVector.size());
    const long configSize = static_cast<long>(configVector.size());
    const long appProfileSize = static_cast<long>(appProfileVector.size());

    const bool sent = writeFully(fd, &dexSize, sizeof(dexSize)) &&
                      writeFully(fd, &configSize, sizeof(configSize)) &&
                      writeFully(fd, &appProfileSize, sizeof(appProfileSize)) &&
                      writeFully(fd, dexVector.data(), dexVector.size()) &&
                      writeFully(fd, configVector.data(), configVector.size()) &&
                      writeFully(fd, appProfileVector.data(), appProfileVector.size());

    if (!sent) LOGD("Companion: failed to send complete payload to Zygisk process");
}

REGISTER_ZYGISK_MODULE(PlayIntegrityFix)

REGISTER_ZYGISK_COMPANION(companion)
