// Per-Game GPU/Device Spoofer - Zygisk module (API v4 style)
//
// Config line format (11 fields):
//   pkg|renderer|vendor|hw|plat|soc|model|manufacturer|brand|device|product
//
// The root companion also writes /data/adb/gpu_spoofer/last.log with one line per
// app that asked for a rule (MATCH / no-rule), readable from the WebUI.

#include <GLES2/gl2.h>
#include <android/dlext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/system_properties.h>
#include <sys/types.h>
#include <unistd.h>

#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "zygisk.hpp"

#define LOG_TAG "GpuSpoofer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

static const char *kConfigPath = "/data/adb/gpu_spoofer/config.txt";
static const char *kLogPath = "/data/adb/gpu_spoofer/last.log";

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static std::string trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::vector<std::string> split(const std::string &s, char d) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == d) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

static std::string lower(std::string s) {
    for (auto &c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

static bool starts_with(const std::string &s, const char *p) {
    return s.compare(0, strlen(p), p) == 0;
}

static bool read_full(int fd, void *buf, size_t n) {
    char *p = static_cast<char *>(buf);
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

static bool write_full(int fd, const void *buf, size_t n) {
    const char *p = static_cast<const char *>(buf);
    while (n > 0) {
        ssize_t r = write(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Rule model
// ---------------------------------------------------------------------------
struct Spoof {
    std::string pkg, renderer, vendor, hw, plat, soc, soc_mfr;
    std::string model, manufacturer, brand, device, product;
    bool active = false;
};
static Spoof g_spoof;

static std::string derive_soc_manufacturer(const Spoof &s) {
    std::string hw = lower(s.hw), soc = lower(s.soc), v = lower(s.vendor);
    if (hw == "qcom" || starts_with(soc, "sm") || starts_with(soc, "sdm") ||
        starts_with(soc, "sxr") || v.find("qualcomm") != std::string::npos)
        return "QTI";
    if (starts_with(hw, "mt") || starts_with(soc, "mt")) return "Mediatek";
    if (starts_with(hw, "ums") || starts_with(hw, "sp") || starts_with(soc, "ums") ||
        starts_with(soc, "t8") || starts_with(soc, "t7"))
        return "UNISOC";
    return s.vendor;
}

static bool parse_rule(const std::string &line, Spoof &out) {
    std::vector<std::string> f = split(line, '|');
    if (f.size() < 11) return false;
    out.pkg = trim(f[0]);
    out.renderer = trim(f[1]);
    out.vendor = trim(f[2]);
    out.hw = trim(f[3]);
    out.plat = trim(f[4]);
    out.soc = trim(f[5]);
    out.model = trim(f[6]);
    out.manufacturer = trim(f[7]);
    out.brand = trim(f[8]);
    out.device = trim(f[9]);
    out.product = trim(f[10]);
    if (out.pkg.empty()) return false;
    out.soc_mfr = derive_soc_manufacturer(out);
    out.active = true;
    return true;
}

// ---------------------------------------------------------------------------
// Root companion: runs as root, can read /data/adb
// Protocol: [u32 len][pkg bytes]  ->  [u32 len][matching config line] (len 0 = no rule)
// ---------------------------------------------------------------------------
static std::string find_rule(const std::string &pkg) {
    std::ifstream in(kConfigPath);
    if (!in.is_open()) return "";
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        std::vector<std::string> f = split(t, '|');
        if (f.size() >= 11 && trim(f[0]) == pkg) return t;
    }
    return "";
}

static void log_event(const std::string &pkg, bool matched) {
    struct stat st;
    if (stat(kLogPath, &st) == 0 && st.st_size > 32768) truncate(kLogPath, 0);
    FILE *f = fopen(kLogPath, "a");
    if (!f) return;
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    fprintf(f, "%s %s %s\n", ts, pkg.c_str(), matched ? "MATCH" : "no-rule");
    fclose(f);
}

static void companion_handler(int fd) {
    uint32_t len = 0;
    if (!read_full(fd, &len, sizeof(len))) return;
    std::string reply;
    if (len > 0 && len <= 512) {
        std::string pkg(len, '\0');
        if (!read_full(fd, &pkg[0], len)) return;
        reply = find_rule(pkg);
        log_event(pkg, !reply.empty());
    }
    uint32_t out = static_cast<uint32_t>(reply.size());
    if (!write_full(fd, &out, sizeof(out))) return;
    if (out > 0) write_full(fd, reply.data(), out);
}

// ---------------------------------------------------------------------------
// Hook state
// ---------------------------------------------------------------------------
typedef const GLubyte *(*glGetString_t)(GLenum);
typedef int (*prop_get_t)(const char *, char *);
typedef void (*prop_cb_t)(void *cookie, const char *name, const char *value, uint32_t serial);
typedef void (*prop_read_cb_t)(const prop_info *, prop_cb_t, void *);
typedef void *(*dlopen_t)(const char *, int);
typedef void *(*dlopen_ext_t)(const char *, int, const android_dlextinfo *);

static glGetString_t g_orig_glGetString = nullptr;
static prop_get_t g_orig_prop_get = nullptr;
static prop_read_cb_t g_orig_prop_read_cb = nullptr;
static dlopen_t g_orig_dlopen = nullptr;
static dlopen_ext_t g_orig_dlopen_ext = nullptr;

static void *g_dummy[8];  // sink for oldFunc, originals are resolved with dlsym
static zygisk::Api *g_api = nullptr;
static std::mutex g_mtx;
static thread_local bool t_in_hook = false;

static const std::string *spoof_prop(const char *name) {
    if (!name || !g_spoof.active) return nullptr;
    const std::string *v = nullptr;
    if (!strcmp(name, "ro.hardware") || !strcmp(name, "ro.boot.hardware"))
        v = &g_spoof.hw;
    else if (!strcmp(name, "ro.board.platform") || !strcmp(name, "ro.product.board"))
        v = &g_spoof.plat;
    else if (!strcmp(name, "ro.soc.model") || !strcmp(name, "ro.hardware.chipname"))
        v = &g_spoof.soc;
    else if (!strcmp(name, "ro.soc.manufacturer"))
        v = &g_spoof.soc_mfr;
    else if (!strcmp(name, "ro.product.model") || !strcmp(name, "ro.product.system.model") ||
             !strcmp(name, "ro.product.vendor.model"))
        v = &g_spoof.model;
    else if (!strcmp(name, "ro.product.manufacturer") ||
             !strcmp(name, "ro.product.system.manufacturer"))
        v = &g_spoof.manufacturer;
    else if (!strcmp(name, "ro.product.brand") || !strcmp(name, "ro.product.system.brand"))
        v = &g_spoof.brand;
    else if (!strcmp(name, "ro.product.device") || !strcmp(name, "ro.product.system.device"))
        v = &g_spoof.device;
    else if (!strcmp(name, "ro.build.product"))
        v = &g_spoof.product;
    if (v && v->empty()) return nullptr;
    return v;
}

static const GLubyte *my_glGetString(GLenum name) {
    if (g_spoof.active) {
        if (name == GL_VENDOR && !g_spoof.vendor.empty())
            return reinterpret_cast<const GLubyte *>(g_spoof.vendor.c_str());
        if (name == GL_RENDERER && !g_spoof.renderer.empty())
            return reinterpret_cast<const GLubyte *>(g_spoof.renderer.c_str());
    }
    return g_orig_glGetString ? g_orig_glGetString(name) : nullptr;
}

static int my_prop_get(const char *name, char *value) {
    int r = g_orig_prop_get ? g_orig_prop_get(name, value) : 0;
    const std::string *v = spoof_prop(name);
    if (v && value) {
        snprintf(value, PROP_VALUE_MAX, "%s", v->c_str());
        return static_cast<int>(strlen(value));
    }
    return r;
}

struct CbCtx {
    prop_cb_t cb;
    void *cookie;
};

static void cb_trampoline(void *cookie, const char *name, const char *value, uint32_t serial) {
    CbCtx *c = static_cast<CbCtx *>(cookie);
    const std::string *v = spoof_prop(name);
    c->cb(c->cookie, name, v ? v->c_str() : value, serial);
}

static void my_prop_read_cb(const prop_info *pi, prop_cb_t cb, void *cookie) {
    if (!g_orig_prop_read_cb || !cb) return;
    CbCtx ctx{cb, cookie};
    g_orig_prop_read_cb(pi, cb_trampoline, &ctx);
}

// ---------------------------------------------------------------------------
// PLT registration (works with both the regex-based and the dev/inode-based API)
// ---------------------------------------------------------------------------
static std::vector<std::pair<dev_t, ino_t>> list_libs() {
    std::vector<std::pair<dev_t, ino_t>> out;
    std::set<std::pair<dev_t, ino_t>> seen;
    FILE *f = fopen("/proc/self/maps", "re");
    if (!f) return out;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        unsigned long start, end, off, ino = 0;
        unsigned maj, min;
        char perms[8];
        char path[512] = "";
        int n = sscanf(line, "%lx-%lx %7s %lx %x:%x %lu %511[^\n]", &start, &end, perms, &off,
                       &maj, &min, &ino, path);
        if (n < 8 || ino == 0 || path[0] != '/') continue;
        std::pair<dev_t, ino_t> key(makedev(maj, min), static_cast<ino_t>(ino));
        if (seen.insert(key).second) out.push_back(key);
    }
    fclose(f);
    return out;
}

template <typename A>
static auto plt_reg(A *api, const char *sym, void *nf, void **of, int)
    -> decltype(api->pltHookRegister(".*", sym, nf, of), void()) {
    api->pltHookRegister(".*", sym, nf, of);
}

template <typename A>
static auto plt_reg(A *api, const char *sym, void *nf, void **of, long)
    -> decltype(api->pltHookRegister(dev_t(), ino_t(), sym, nf, of), void()) {
    for (const auto &lib : list_libs()) api->pltHookRegister(lib.first, lib.second, sym, nf, of);
}

static void *my_dlopen(const char *file, int flags);
static void *my_dlopen_ext(const char *file, int flags, const android_dlextinfo *info);

static void resolve_originals() {
    if (!g_orig_glGetString)
        g_orig_glGetString = reinterpret_cast<glGetString_t>(dlsym(RTLD_DEFAULT, "glGetString"));
    if (!g_orig_prop_get)
        g_orig_prop_get = reinterpret_cast<prop_get_t>(dlsym(RTLD_DEFAULT, "__system_property_get"));
    if (!g_orig_prop_read_cb)
        g_orig_prop_read_cb = reinterpret_cast<prop_read_cb_t>(
            dlsym(RTLD_DEFAULT, "__system_property_read_callback"));
    if (!g_orig_dlopen) g_orig_dlopen = reinterpret_cast<dlopen_t>(dlsym(RTLD_DEFAULT, "dlopen"));
    if (!g_orig_dlopen_ext)
        g_orig_dlopen_ext =
            reinterpret_cast<dlopen_ext_t>(dlsym(RTLD_DEFAULT, "android_dlopen_ext"));
}

static void apply_hooks() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_api) return;
    resolve_originals();
    plt_reg(g_api, "glGetString", reinterpret_cast<void *>(my_glGetString), &g_dummy[0], 0);
    plt_reg(g_api, "__system_property_get", reinterpret_cast<void *>(my_prop_get), &g_dummy[1], 0);
    plt_reg(g_api, "__system_property_read_callback", reinterpret_cast<void *>(my_prop_read_cb),
            &g_dummy[2], 0);
    plt_reg(g_api, "dlopen", reinterpret_cast<void *>(my_dlopen), &g_dummy[3], 0);
    plt_reg(g_api, "android_dlopen_ext", reinterpret_cast<void *>(my_dlopen_ext), &g_dummy[4], 0);
    g_api->pltHookCommit();
}

static void after_load(void *handle) {
    if (!handle || t_in_hook) return;
    t_in_hook = true;
    apply_hooks();  // hook libraries that were just loaded by the game
    t_in_hook = false;
}

static void *my_dlopen(const char *file, int flags) {
    void *h = g_orig_dlopen ? g_orig_dlopen(file, flags) : nullptr;
    after_load(h);
    return h;
}

static void *my_dlopen_ext(const char *file, int flags, const android_dlextinfo *info) {
    void *h = g_orig_dlopen_ext ? g_orig_dlopen_ext(file, flags, info) : nullptr;
    after_load(h);
    return h;
}

// ---------------------------------------------------------------------------
// android.os.Build overwrite
// ---------------------------------------------------------------------------
static void set_build_field(JNIEnv *env, jclass cls, const char *field, const std::string &val) {
    if (val.empty()) return;
    jfieldID fid = env->GetStaticFieldID(cls, field, "Ljava/lang/String;");
    if (!fid) {
        env->ExceptionClear();  // e.g. SOC_* does not exist below Android 12
        return;
    }
    jstring s = env->NewStringUTF(val.c_str());
    if (!s) {
        env->ExceptionClear();
        return;
    }
    env->SetStaticObjectField(cls, fid, s);
    env->DeleteLocalRef(s);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

static void override_build(JNIEnv *env) {
    jclass cls = env->FindClass("android/os/Build");
    if (!cls) {
        env->ExceptionClear();
        return;
    }
    set_build_field(env, cls, "HARDWARE", g_spoof.hw);
    set_build_field(env, cls, "BOARD", g_spoof.plat);
    set_build_field(env, cls, "SOC_MANUFACTURER", g_spoof.soc_mfr);
    set_build_field(env, cls, "SOC_MODEL", g_spoof.soc);
    set_build_field(env, cls, "MODEL", g_spoof.model);
    set_build_field(env, cls, "MANUFACTURER", g_spoof.manufacturer);
    set_build_field(env, cls, "BRAND", g_spoof.brand);
    set_build_field(env, cls, "DEVICE", g_spoof.device);
    set_build_field(env, cls, "PRODUCT", g_spoof.product);
    env->DeleteLocalRef(cls);
}

// ---------------------------------------------------------------------------
// Zygisk module
// ---------------------------------------------------------------------------
static std::string jstr(JNIEnv *env, jstring s) {
    if (!s) return "";
    const char *c = env->GetStringUTFChars(s, nullptr);
    if (!c) return "";
    std::string r(c);
    env->ReleaseStringUTFChars(s, c);
    return r;
}

void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        matched_ = false;
        std::string name = jstr(env_, args->nice_name);
        size_t colon = name.find(':');
        if (colon != std::string::npos) name.resize(colon);  // com.pkg:service -> com.pkg

        if (!name.empty()) {
            std::string line = query_companion(name);
            if (!line.empty() && parse_rule(line, g_spoof)) matched_ = true;
        }
        if (!matched_) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!matched_) return;
        LOGI("spoofing %s -> render=%s/%s hw=%s plat=%s soc=%s model=%s mfr=%s brand=%s dev=%s prod=%s",
             g_spoof.pkg.c_str(), g_spoof.renderer.c_str(), g_spoof.vendor.c_str(),
             g_spoof.hw.c_str(), g_spoof.plat.c_str(), g_spoof.soc.c_str(), g_spoof.model.c_str(),
             g_spoof.manufacturer.c_str(), g_spoof.brand.c_str(), g_spoof.device.c_str(),
             g_spoof.product.c_str());
        override_build(env_);
        g_api = api_;
        apply_hooks();
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs *) override {
        api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }

private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool matched_ = false;

    std::string query_companion(const std::string &pkg) {
        int fd = api_->connectCompanion();
        if (fd < 0) {
            LOGW("connectCompanion failed");
            return "";
        }
        std::string result;
        uint32_t len = static_cast<uint32_t>(pkg.size());
        uint32_t rlen = 0;
        if (write_full(fd, &len, sizeof(len)) && write_full(fd, pkg.data(), len) &&
            read_full(fd, &rlen, sizeof(rlen)) && rlen > 0 && rlen <= 4096) {
            result.resize(rlen);
            if (!read_full(fd, &result[0], rlen)) result.clear();
        }
        close(fd);
        return result;
    }
};

REGISTER_ZYGISK_MODULE(GpuSpooferModule)
REGISTER_ZYGISK_COMPANION(companion_handler)
