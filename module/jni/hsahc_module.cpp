#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include <stdint.h>
#include <string.h>

#include "zygisk.hpp"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

namespace {

constexpr const char *kLogTag = "hsahc-zygisk";
constexpr const char *kTargetProcess = "com.lta.hsahc.aligames";
constexpr const char *kIl2cppSo = "libil2cpp.so";
constexpr const char *kLogDir = "/data/adb/hsahc_forceupdate_zygisk";
constexpr const char *kLogPath = "/data/adb/hsahc_forceupdate_zygisk/runtime.log";
constexpr const char *kFallbackLogPath = "/data/user/0/com.lta.hsahc.aligames/files/hsahc_zygisk.log";
constexpr int kMaxRetry = 180;
constexpr int kRetrySleepSec = 1;
int gLogFd = -1;

void writeFileLog(const char *level, const char *message) {
  if (gLogFd < 0 || level == nullptr || message == nullptr) {
    return;
  }

  char timebuf[32] = {0};
  time_t now = time(nullptr);
  struct tm tmv {};
  localtime_r(&now, &tmv);
  strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tmv);

  char line[2300] = {0};
  int n = snprintf(line, sizeof(line), "%s [%s] %s\n", timebuf, level, message);
  if (n > 0) {
    write(gLogFd, line, static_cast<size_t>(n));
  }
}

void logPrint(int prio, const char *level, const char *fmt, ...) {
  char msg[2048] = {0};
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  __android_log_print(prio, kLogTag, "%s", msg);
  writeFileLog(level, msg);
}

#define LOGI(...) logPrint(ANDROID_LOG_INFO, "信息", __VA_ARGS__)
#define LOGE(...) logPrint(ANDROID_LOG_ERROR, "错误", __VA_ARGS__)

void ensureLogFileReady() {
  if (gLogFd >= 0) {
    return;
  }

  mkdir(kLogDir, 0755);
  gLogFd = open(kLogPath, O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
  if (gLogFd >= 0) {
    LOGI("日志文件已打开: %s", kLogPath);
    return;
  }

  gLogFd = open(kFallbackLogPath, O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
  if (gLogFd >= 0) {
    LOGI("日志文件已打开(回退路径): %s", kFallbackLogPath);
    return;
  }

  __android_log_print(ANDROID_LOG_ERROR, kLogTag, "无法打开日志文件, errno=%d", errno);
}

using il2cpp_domain_get_t = void *(*)();
using il2cpp_thread_attach_t = void *(*)(void *);
using il2cpp_domain_get_assemblies_t = const void **(*)(void *, size_t *);
using il2cpp_assembly_get_image_t = void *(*)(const void *);
using il2cpp_image_get_name_t = const char *(*)(void *);
using il2cpp_image_get_class_count_t = size_t (*)(void *);
using il2cpp_image_get_class_t = void *(*)(void *, size_t);
using il2cpp_class_get_name_t = const char *(*)(void *);
using il2cpp_class_get_namespace_t = const char *(*)(void *);
using il2cpp_class_get_methods_t = const void *(*)(void *, void **);
using il2cpp_method_get_name_t = const char *(*)(const void *);
using il2cpp_method_get_param_count_t = uint32_t (*)(const void *);

struct Il2CppApi {
  il2cpp_domain_get_t domain_get = nullptr;
  il2cpp_thread_attach_t thread_attach = nullptr;
  il2cpp_domain_get_assemblies_t domain_get_assemblies = nullptr;
  il2cpp_assembly_get_image_t assembly_get_image = nullptr;
  il2cpp_image_get_name_t image_get_name = nullptr;
  il2cpp_image_get_class_count_t image_get_class_count = nullptr;
  il2cpp_image_get_class_t image_get_class = nullptr;
  il2cpp_class_get_name_t class_get_name = nullptr;
  il2cpp_class_get_namespace_t class_get_namespace = nullptr;
  il2cpp_class_get_methods_t class_get_methods = nullptr;
  il2cpp_method_get_name_t method_get_name = nullptr;
  il2cpp_method_get_param_count_t method_get_param_count = nullptr;
};

struct MethodInfoPatch {
  void *methodPointer;
  void *virtualMethodPointer;
};

struct TargetSpec {
  const char *method;
  const char *classKeyword;
  int action;
  int hitCount;
};

extern "C" bool ret_false_stub(...) { return false; }
extern "C" int ret_zero_stub(...) { return 0; }
extern "C" void ret_void_stub(...) {}

enum ReplaceAction : int {
  kReturnFalse = 0,
  kReturnZero = 1,
  kReturnVoid = 2,
};

TargetSpec gTargets[] = {
  {"IsVersionLessThanTargetVersion", "GameMgr", kReturnFalse, 0},
  {"VersionCompare", "GameMgr", kReturnZero, 0},
  {"ConfirmVersionForceUpdateJumpCallback", "GameMgr", kReturnVoid, 0},
};

void *resolveActionPtr(int action) {
  switch (action) {
    case kReturnFalse:
      return reinterpret_cast<void *>(ret_false_stub);
    case kReturnZero:
      return reinterpret_cast<void *>(ret_zero_stub);
    case kReturnVoid:
      return reinterpret_cast<void *>(ret_void_stub);
    default:
      return nullptr;
  }
}

bool makeWritable(void *addr) {
  long pageSize = sysconf(_SC_PAGESIZE);
  if (pageSize <= 0) {
    return false;
  }
  uintptr_t page = reinterpret_cast<uintptr_t>(addr) & ~(static_cast<uintptr_t>(pageSize) - 1U);
  if (mprotect(reinterpret_cast<void *>(page), static_cast<size_t>(pageSize), PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    return false;
  }
  return true;
}

bool patchMethodPointer(const void *methodInfo, void *replace, const char *klass, const char *method, uint32_t paramCount) {
  auto *m = reinterpret_cast<MethodInfoPatch *>(const_cast<void *>(methodInfo));
  if (m == nullptr || m->methodPointer == nullptr) {
    return false;
  }
  if (m->methodPointer == replace) {
    return true;
  }
  if (!makeWritable(m)) {
    LOGE("mprotect 失败: %s::%s", klass, method);
    return false;
  }
  void *old = m->methodPointer;
  m->methodPointer = replace;
  if (m->virtualMethodPointer != nullptr) {
    m->virtualMethodPointer = replace;
  }
  __builtin___clear_cache(reinterpret_cast<char *>(m), reinterpret_cast<char *>(m) + sizeof(MethodInfoPatch));
  LOGI("已补丁 %s::%s(%u) 原始=%p 新值=%p", klass, method, paramCount, old, replace);
  return true;
}

bool resolveIl2cpp(Il2CppApi &api) {
  void *h = dlopen(kIl2cppSo, RTLD_NOW | RTLD_NOLOAD);
  if (h == nullptr) {
    return false;
  }
#define RESOLVE(name, sym)                                           \
  do {                                                               \
    api.name = reinterpret_cast<name##_t>(dlsym(h, sym));           \
    if (api.name == nullptr) {                                       \
      LOGE("缺少 il2cpp 导出符号: %s", sym);                         \
      return false;                                                  \
    }                                                                \
  } while (0)

  RESOLVE(domain_get, "il2cpp_domain_get");
  RESOLVE(thread_attach, "il2cpp_thread_attach");
  RESOLVE(domain_get_assemblies, "il2cpp_domain_get_assemblies");
  RESOLVE(assembly_get_image, "il2cpp_assembly_get_image");
  RESOLVE(image_get_name, "il2cpp_image_get_name");
  RESOLVE(image_get_class_count, "il2cpp_image_get_class_count");
  RESOLVE(image_get_class, "il2cpp_image_get_class");
  RESOLVE(class_get_name, "il2cpp_class_get_name");
  RESOLVE(class_get_namespace, "il2cpp_class_get_namespace");
  RESOLVE(class_get_methods, "il2cpp_class_get_methods");
  RESOLVE(method_get_name, "il2cpp_method_get_name");
  RESOLVE(method_get_param_count, "il2cpp_method_get_param_count");
#undef RESOLVE
  return true;
}

bool classMatch(const char *klass, const char *ns, const char *keyword) {
  if (keyword == nullptr || keyword[0] == '\0') {
    return true;
  }
  if (klass != nullptr && strstr(klass, keyword) != nullptr) {
    return true;
  }
  if (ns != nullptr && strstr(ns, keyword) != nullptr) {
    return true;
  }
  return false;
}

int patchTargets(Il2CppApi &api, bool strictClass) {
  void *domain = api.domain_get();
  if (domain == nullptr) {
    return 0;
  }
  api.thread_attach(domain);

  size_t asmCount = 0;
  const void **assemblies = api.domain_get_assemblies(domain, &asmCount);
  if (assemblies == nullptr || asmCount == 0) {
    return 0;
  }

  int patched = 0;
  for (size_t i = 0; i < asmCount; i++) {
    const void *assembly = assemblies[i];
    if (assembly == nullptr) {
      continue;
    }
    void *image = api.assembly_get_image(assembly);
    if (image == nullptr) {
      continue;
    }
    const char *imgName = api.image_get_name(image);
    if (imgName == nullptr || strstr(imgName, "Assembly-CSharp") == nullptr) {
      continue;
    }

    size_t classCount = api.image_get_class_count(image);
    for (size_t c = 0; c < classCount; c++) {
      void *klass = api.image_get_class(image, c);
      if (klass == nullptr) {
        continue;
      }
      const char *klassName = api.class_get_name(klass);
      const char *klassNs = api.class_get_namespace(klass);

      void *iter = nullptr;
      while (true) {
        const void *methodInfo = api.class_get_methods(klass, &iter);
        if (methodInfo == nullptr) {
          break;
        }
        const char *methodName = api.method_get_name(methodInfo);
        if (methodName == nullptr) {
          continue;
        }
        uint32_t paramCount = api.method_get_param_count(methodInfo);

        for (auto &t : gTargets) {
          if (strcmp(methodName, t.method) != 0) {
            continue;
          }
          if (strictClass && !classMatch(klassName, klassNs, t.classKeyword)) {
            continue;
          }
          void *replace = resolveActionPtr(t.action);
          if (replace == nullptr) {
            continue;
          }
          if (patchMethodPointer(methodInfo, replace, klassName ? klassName : "<null>", methodName, paramCount)) {
            t.hitCount++;
            patched++;
          }
        }
      }
    }
  }
  return patched;
}

void *workerThread(void *) {
  for (int i = 0; i < kMaxRetry; i++) {
    Il2CppApi api{};
    if (!resolveIl2cpp(api)) {
      sleep(kRetrySleepSec);
      continue;
    }

    int strictPatched = patchTargets(api, true);
    int fallbackPatched = 0;
    if (strictPatched == 0) {
      fallbackPatched = patchTargets(api, false);
    }

    int total = strictPatched + fallbackPatched;
    if (total > 0) {
      LOGI("il2cpp 强更绕过已生效, 命中总数=%d (严格匹配=%d, 回退匹配=%d)", total, strictPatched, fallbackPatched);
      return nullptr;
    }

    sleep(kRetrySleepSec);
  }

  LOGE("在超时窗口内未能补丁 il2cpp 目标方法");
  return nullptr;
}

}  // namespace

class HsahcZygiskModule : public zygisk::ModuleBase {
public:
  void onLoad(Api *api, JNIEnv *env) override {
    api_ = api;
    env_ = env;
  }

  void preAppSpecialize(AppSpecializeArgs *args) override {
    target_ = false;
    if (args != nullptr && args->nice_name != nullptr) {
      const char *name = env_->GetStringUTFChars(args->nice_name, nullptr);
      if (name != nullptr) {
        target_ = (strcmp(name, kTargetProcess) == 0);
        env_->ReleaseStringUTFChars(args->nice_name, name);
      }
    }
    if (!target_) {
      api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    } else {
      ensureLogFileReady();
      LOGI("命中目标进程，开始准备 il2cpp 补丁");
    }
  }

  void postAppSpecialize(const AppSpecializeArgs *args) override {
    (void) args;
    if (!target_) {
      return;
    }
    pthread_t t{};
    if (pthread_create(&t, nullptr, workerThread, nullptr) == 0) {
      pthread_detach(t);
    } else {
      LOGE("pthread_create 失败");
    }
  }

  void preServerSpecialize(ServerSpecializeArgs *args) override {
    (void) args;
    api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
  }

private:
  Api *api_ = nullptr;
  JNIEnv *env_ = nullptr;
  bool target_ = false;
};

REGISTER_ZYGISK_MODULE(HsahcZygiskModule)
