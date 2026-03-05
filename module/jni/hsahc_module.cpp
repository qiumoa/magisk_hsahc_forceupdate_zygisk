#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "zygisk.hpp"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

namespace {

constexpr const char *kLogTag = "hsahc-zygisk";
constexpr const char *kTargetProcess = "com.lta.hsahc.aligames";
constexpr const char *kIl2cppSo = "libil2cpp.so";

constexpr const char *kLogDir = "/data/adb/hsahc_forceupdate_zygisk";
constexpr const char *kAdbLogPath = "/data/adb/hsahc_forceupdate_zygisk/runtime.log";
constexpr const char *kAppLogPath = "/data/user/0/com.lta.hsahc.aligames/files/hsahc_zygisk.log";
constexpr const char *kLastProcPath = "/data/adb/hsahc_forceupdate_zygisk/last_process.log";
constexpr const char *kCfgPathModule = "/data/adb/modules/hsahc_forceupdate_zygisk/config.prop";
constexpr const char *kCfgPathData = "/data/adb/hsahc_forceupdate_zygisk/config.prop";

constexpr int kMaxRetry = 180;
constexpr int kRetrySleepSec = 1;
constexpr int kDefaultPatchDelaySec = 10;
constexpr int kMinPatchDelaySec = 2;
constexpr int kMaxPatchDelaySec = 60;

int gLogFd = -1;
int gLastProcFd = -1;
int gPatchDelaySec = kDefaultPatchDelaySec;

void writeLineFd(int fd, const char *line) {
  if (fd < 0 || line == nullptr) {
    return;
  }
  size_t n = strlen(line);
  if (n > 0) {
    write(fd, line, n);
  }
}

void makeTimeStr(char *out, size_t outSize) {
  if (out == nullptr || outSize == 0) {
    return;
  }
  time_t now = time(nullptr);
  struct tm tmv {};
  localtime_r(&now, &tmv);
  strftime(out, outSize, "%Y-%m-%d %H:%M:%S", &tmv);
}

void writeFileLog(const char *level, const char *message) {
  if (gLogFd < 0 || level == nullptr || message == nullptr) {
    return;
  }
  char t[32] = {0};
  makeTimeStr(t, sizeof(t));
  char line[2048] = {0};
  int n = snprintf(line, sizeof(line), "%s [%s] %s\n", t, level, message);
  if (n > 0) {
    writeLineFd(gLogFd, line);
  }
}

void logPrint(int prio, const char *level, const char *fmt, ...) {
  char msg[1600] = {0};
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  __android_log_print(prio, kLogTag, "%s", msg);
  writeFileLog(level, msg);
}

#define LOGI(...) logPrint(ANDROID_LOG_INFO, "INFO", __VA_ARGS__)
#define LOGE(...) logPrint(ANDROID_LOG_ERROR, "ERROR", __VA_ARGS__)

bool jstrToBuf(JNIEnv *env, jstring js, char *out, size_t outSize) {
  if (env == nullptr || js == nullptr || out == nullptr || outSize == 0) {
    return false;
  }
  out[0] = '\0';
  const char *s = env->GetStringUTFChars(js, nullptr);
  if (s == nullptr) {
    return false;
  }
  strncpy(out, s, outSize - 1);
  out[outSize - 1] = '\0';
  env->ReleaseStringUTFChars(js, s);
  return out[0] != '\0';
}

bool isTargetProcessName(const char *name) {
  if (name == nullptr) {
    return false;
  }
  size_t n = strlen(kTargetProcess);
  if (strncmp(name, kTargetProcess, n) != 0) {
    return false;
  }
  char tail = name[n];
  return tail == '\0' || tail == ':' || tail == '.';
}

bool isTargetByDataDir(const char *appDataDir) {
  return appDataDir != nullptr && strstr(appDataDir, kTargetProcess) != nullptr;
}

void recordLastProcess(jint uid, const char *name, const char *isa, const char *appDataDir, bool target) {
  if (gLastProcFd < 0) {
    mkdir(kLogDir, 0755);
    gLastProcFd = open(kLastProcPath, O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
  }
  if (gLastProcFd < 0) {
    return;
  }
  char t[32] = {0};
  makeTimeStr(t, sizeof(t));
  const char *n = (name && name[0]) ? name : "-";
  const char *i = (isa && isa[0]) ? isa : "-";
  const char *d = (appDataDir && appDataDir[0]) ? appDataDir : "-";
  char line[1024] = {0};
  int c = snprintf(line, sizeof(line), "%s uid=%d name=%s isa=%s dir=%s target=%s\n", t, static_cast<int>(uid), n,
                   i, d, target ? "1" : "0");
  if (c > 0) {
    writeLineFd(gLastProcFd, line);
  }
}

void ensureLogFileReady() {
  if (gLogFd >= 0) {
    return;
  }
  gLogFd = open(kAppLogPath, O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
  if (gLogFd >= 0) {
    LOGI("log opened: %s", kAppLogPath);
    return;
  }
  mkdir(kLogDir, 0755);
  gLogFd = open(kAdbLogPath, O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
  if (gLogFd >= 0) {
    LOGI("log opened(fallback): %s", kAdbLogPath);
    return;
  }
  __android_log_print(ANDROID_LOG_ERROR, kLogTag, "open log failed errno=%d", errno);
}

int clampInt(int v, int lo, int hi) {
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

bool parseDelayConfigLine(const char *line, int *outValue) {
  if (line == nullptr || outValue == nullptr) {
    return false;
  }
  while (*line == ' ' || *line == '\t') {
    ++line;
  }
  if (strncmp(line, "patch_delay_sec", 15) != 0) {
    return false;
  }
  const char *eq = strchr(line, '=');
  if (eq == nullptr) {
    return false;
  }
  ++eq;
  while (*eq == ' ' || *eq == '\t') {
    ++eq;
  }
  if (*eq < '0' || *eq > '9') {
    return false;
  }
  *outValue = atoi(eq);
  return true;
}

int loadPatchDelayConfig() {
  const char *paths[] = {kCfgPathModule, kCfgPathData};
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
    FILE *fp = fopen(paths[i], "r");
    if (fp == nullptr) {
      continue;
    }
    char line[256] = {0};
    int value = kDefaultPatchDelaySec;
    bool found = false;
    while (fgets(line, sizeof(line), fp) != nullptr) {
      if (parseDelayConfigLine(line, &value)) {
        found = true;
        break;
      }
    }
    fclose(fp);
    if (found) {
      int c = clampInt(value, kMinPatchDelaySec, kMaxPatchDelaySec);
      __android_log_print(ANDROID_LOG_INFO, kLogTag, "load config: %s patch_delay_sec=%d", paths[i], c);
      return c;
    }
  }
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "use default patch_delay_sec=%d", kDefaultPatchDelaySec);
  return kDefaultPatchDelaySec;
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
};

enum ReplaceAction : int {
  kReturnFalse = 0,
  kReturnZero = 1,
  kReturnVoid = 2,
  kReturnTrue = 3,
};

struct TargetSpec {
  const char *method;
  const char *classKeyword;
  int action;
  int hitCount;
};

extern "C" bool ret_false_stub(...) { return false; }
extern "C" bool ret_true_stub(...) { return true; }
extern "C" int ret_zero_stub(...) { return 0; }
extern "C" void ret_void_stub(...) {}

TargetSpec gTargets[] = {
    {"get_IsGameControlPassed", "GameMgr", kReturnTrue, 0},
    {"IsGameControlPassed", "GameMgr", kReturnTrue, 0},
    {"set_IsGameControlPassed", "GameMgr", kReturnVoid, 0},
    {"RequestGameControl", "GameMgr", kReturnVoid, 0},
    {"RequestGameControlCallback", "GameMgr", kReturnVoid, 0},
    {"RequestVersionControl", "", kReturnVoid, 0},
    {"CancelReqeustGameControl", "GameMgr", kReturnVoid, 0},
    {"IsVersionLessThanTargetVersion", "GameMgr", kReturnFalse, 0},
    {"VersionCompare", "GameMgr", kReturnZero, 0},
    {"ConfirmVersionForceUpdateJumpCallback", "GameMgr", kReturnVoid, 0},
    {"VersionForceUpdateJump", "", kReturnVoid, 0},
    {"versionForceUpdateJump", "", kReturnVoid, 0},
    {"OpenNativeBrowser", "LTUnityCallNative", kReturnVoid, 0},
    {"ShowNativeQuitDialog", "LTUnityCallNative", kReturnVoid, 0},
};

void *resolveActionPtr(int action) {
  switch (action) {
    case kReturnFalse:
      return reinterpret_cast<void *>(ret_false_stub);
    case kReturnTrue:
      return reinterpret_cast<void *>(ret_true_stub);
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
  return mprotect(reinterpret_cast<void *>(page), static_cast<size_t>(pageSize),
                  PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
}

bool findLoadedSoPath(const char *soname, char *out, size_t outSize) {
  if (soname == nullptr || out == nullptr || outSize == 0) {
    return false;
  }
  out[0] = '\0';
  FILE *fp = fopen("/proc/self/maps", "r");
  if (fp == nullptr) {
    return false;
  }
  char line[1024] = {0};
  bool found = false;
  while (fgets(line, sizeof(line), fp) != nullptr) {
    const char *p = strstr(line, soname);
    if (p == nullptr) {
      continue;
    }
    while (p > line && *(p - 1) != ' ') {
      --p;
    }
    size_t len = strcspn(p, "\r\n");
    if (len == 0 || len >= outSize) {
      continue;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    found = true;
    break;
  }
  fclose(fp);
  return found;
}

bool findLibraryBase(const char *fullPath, uintptr_t *baseOut) {
  if (fullPath == nullptr || baseOut == nullptr) {
    return false;
  }
  FILE *fp = fopen("/proc/self/maps", "r");
  if (fp == nullptr) {
    return false;
  }
  bool found = false;
  uintptr_t best = 0;
  char line[1024] = {0};
  while (fgets(line, sizeof(line), fp) != nullptr) {
    if (strstr(line, fullPath) == nullptr) {
      continue;
    }
    unsigned long long start = 0;
    unsigned long long end = 0;
    unsigned long long offset = 0;
    char perms[8] = {0};
    if (sscanf(line, "%llx-%llx %7s %llx", &start, &end, perms, &offset) < 4) {
      continue;
    }
    uintptr_t base = static_cast<uintptr_t>(start - offset);
    if (!found || base < best) {
      best = base;
      found = true;
    }
  }
  fclose(fp);
  if (!found) {
    return false;
  }
  *baseOut = best;
  return true;
}

bool preadAll(int fd, void *buf, size_t size, off_t off) {
  char *p = reinterpret_cast<char *>(buf);
  size_t done = 0;
  while (done < size) {
    ssize_t n = pread(fd, p + done, size - done, off + static_cast<off_t>(done));
    if (n <= 0) {
      return false;
    }
    done += static_cast<size_t>(n);
  }
  return true;
}

bool resolveElfSymOffset64(const char *path, const char *symName, uintptr_t *symOffsetOut) {
  if (path == nullptr || symName == nullptr || symOffsetOut == nullptr) {
    return false;
  }
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }

  Elf64_Ehdr eh {};
  if (!preadAll(fd, &eh, sizeof(eh), 0)) {
    close(fd);
    return false;
  }
  if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_shoff == 0 ||
      eh.e_shentsize != sizeof(Elf64_Shdr) || eh.e_shnum == 0) {
    close(fd);
    return false;
  }

  size_t shdrSize = static_cast<size_t>(eh.e_shnum) * sizeof(Elf64_Shdr);
  auto *shdrs = reinterpret_cast<Elf64_Shdr *>(malloc(shdrSize));
  if (shdrs == nullptr) {
    close(fd);
    return false;
  }
  if (!preadAll(fd, shdrs, shdrSize, static_cast<off_t>(eh.e_shoff))) {
    free(shdrs);
    close(fd);
    return false;
  }

  bool found = false;
  uintptr_t offset = 0;
  for (uint16_t i = 0; i < eh.e_shnum && !found; ++i) {
    Elf64_Shdr &symSec = shdrs[i];
    if (!(symSec.sh_type == SHT_DYNSYM || symSec.sh_type == SHT_SYMTAB) || symSec.sh_entsize != sizeof(Elf64_Sym)) {
      continue;
    }
    if (symSec.sh_link >= eh.e_shnum) {
      continue;
    }
    Elf64_Shdr &strSec = shdrs[symSec.sh_link];
    if (strSec.sh_size == 0) {
      continue;
    }
    auto *strtab = reinterpret_cast<char *>(malloc(static_cast<size_t>(strSec.sh_size)));
    if (strtab == nullptr) {
      continue;
    }
    if (!preadAll(fd, strtab, static_cast<size_t>(strSec.sh_size), static_cast<off_t>(strSec.sh_offset))) {
      free(strtab);
      continue;
    }

    size_t symCount = static_cast<size_t>(symSec.sh_size / sizeof(Elf64_Sym));
    Elf64_Sym sym {};
    for (size_t k = 0; k < symCount; ++k) {
      off_t sOff = static_cast<off_t>(symSec.sh_offset + k * sizeof(Elf64_Sym));
      if (!preadAll(fd, &sym, sizeof(sym), sOff)) {
        break;
      }
      if (sym.st_name >= strSec.sh_size) {
        continue;
      }
      const char *name = strtab + sym.st_name;
      if (name == nullptr || *name == '\0') {
        continue;
      }
      if (strcmp(name, symName) != 0 || sym.st_value == 0) {
        continue;
      }
      offset = static_cast<uintptr_t>(sym.st_value);
      found = true;
      break;
    }
    free(strtab);
  }

  free(shdrs);
  close(fd);
  if (!found) {
    return false;
  }
  *symOffsetOut = offset;
  return true;
}

bool resolveOneSymbol(const char *soPath, uintptr_t soBase, const char *sym, void **out) {
  void *p = dlsym(RTLD_DEFAULT, sym);
  if (p != nullptr) {
    memcpy(out, &p, sizeof(p));
    return true;
  }

  if (soPath == nullptr || soBase == 0) {
    return false;
  }
  uintptr_t off = 0;
  if (!resolveElfSymOffset64(soPath, sym, &off) || off == 0) {
    return false;
  }
  p = reinterpret_cast<void *>(soBase + off);
  memcpy(out, &p, sizeof(p));
  return true;
}

bool resolveIl2cpp(Il2CppApi &api) {
  char soPath[512] = {0};
  uintptr_t soBase = 0;
  if (!findLoadedSoPath(kIl2cppSo, soPath, sizeof(soPath)) || !findLibraryBase(soPath, &soBase)) {
    return false;
  }

  if (!resolveOneSymbol(soPath, soBase, "il2cpp_domain_get", reinterpret_cast<void **>(&api.domain_get))) return false;
  if (!resolveOneSymbol(soPath, soBase, "il2cpp_thread_attach", reinterpret_cast<void **>(&api.thread_attach)))
    return false;
  if (!resolveOneSymbol(soPath, soBase, "il2cpp_domain_get_assemblies",
                        reinterpret_cast<void **>(&api.domain_get_assemblies)))
    return false;
  if (!resolveOneSymbol(soPath, soBase, "il2cpp_assembly_get_image", reinterpret_cast<void **>(&api.assembly_get_image)))
    return false;
  if (!resolveOneSymbol(soPath, soBase, "il2cpp_image_get_name", reinterpret_cast<void **>(&api.image_get_name)))
    return false;
  if (!resolveOneSymbol(soPath, soBase, "il2cpp_image_get_class_count",
                        reinterpret_cast<void **>(&api.image_get_class_count)))
    return false;
  if (!resolveOneSymbol(soPath, soBase, "il2cpp_image_get_class", reinterpret_cast<void **>(&api.image_get_class)))
    return false;
  if (!resolveOneSymbol(soPath, soBase, "il2cpp_class_get_name", reinterpret_cast<void **>(&api.class_get_name)))
    return false;
  if (!resolveOneSymbol(soPath, soBase, "il2cpp_class_get_namespace",
                        reinterpret_cast<void **>(&api.class_get_namespace)))
    return false;
  if (!resolveOneSymbol(soPath, soBase, "il2cpp_class_get_methods", reinterpret_cast<void **>(&api.class_get_methods)))
    return false;
  if (!resolveOneSymbol(soPath, soBase, "il2cpp_method_get_name", reinterpret_cast<void **>(&api.method_get_name)))
    return false;
  if (!resolveOneSymbol(soPath, soBase, "il2cpp_method_get_param_count",
                        reinterpret_cast<void **>(&api.method_get_param_count)))
    return false;

  static bool logged = false;
  if (!logged) {
    LOGI("il2cpp resolved via ELF fallback");
    logged = true;
  }
  return true;
}

bool patchMethodPointer(const void *methodInfo, void *replace, const char *klass, const char *method,
                        uint32_t paramCount) {
  auto *m = reinterpret_cast<MethodInfoPatch *>(const_cast<void *>(methodInfo));
  if (m == nullptr || m->methodPointer == nullptr || replace == nullptr) {
    return false;
  }
  if (m->methodPointer == replace) {
    return false;
  }
  if (!makeWritable(m)) {
    LOGE("mprotect failed: %s::%s", klass, method);
    return false;
  }
  void *old = m->methodPointer;
  m->methodPointer = replace;
  __builtin___clear_cache(reinterpret_cast<char *>(m), reinterpret_cast<char *>(m + 1));
  LOGI("patched %s::%s(%u) old=%p new=%p", klass, method, paramCount, old, replace);
  return true;
}

bool classMatch(const char *klass, const char *nameSpace, const char *keyword) {
  if (keyword == nullptr || keyword[0] == '\0') {
    return true;
  }
  if (klass != nullptr && strstr(klass, keyword) != nullptr) {
    return true;
  }
  if (nameSpace != nullptr && strstr(nameSpace, keyword) != nullptr) {
    return true;
  }
  return false;
}

bool classNameContains(const char *klass, const char *nameSpace, const char *keyword) {
  if (keyword == nullptr || keyword[0] == '\0') {
    return false;
  }
  if (klass != nullptr && strstr(klass, keyword) != nullptr) {
    return true;
  }
  if (nameSpace != nullptr && strstr(nameSpace, keyword) != nullptr) {
    return true;
  }
  return false;
}

bool methodNameContainsAny(const char *methodName, const char *const *keys, int n) {
  if (methodName == nullptr || keys == nullptr || n <= 0) {
    return false;
  }
  for (int i = 0; i < n; i++) {
    if (keys[i] != nullptr && keys[i][0] != '\0' && strstr(methodName, keys[i]) != nullptr) {
      return true;
    }
  }
  return false;
}

void logCandidateMethods(Il2CppApi &api) {
  static bool dumped = false;
  if (dumped) {
    return;
  }

  void *domain = api.domain_get();
  if (domain == nullptr) {
    return;
  }
  api.thread_attach(domain);

  size_t asmCount = 0;
  const void **assemblies = api.domain_get_assemblies(domain, &asmCount);
  if (assemblies == nullptr || asmCount == 0) {
    return;
  }

  const char *classKeys[] = {"GameMgr", "LTUnityCallNative", "UpdateVersionUI"};
  const char *methodKeys[] = {"Version", "Update", "Force", "GameControl", "version", "update", "force"};
  int printed = 0;
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
      bool classOk = false;
      for (int k = 0; k < 3; k++) {
        if (classNameContains(klassName, klassNs, classKeys[k])) {
          classOk = true;
          break;
        }
      }
      if (!classOk) {
        continue;
      }

      void *iter = nullptr;
      while (true) {
        const void *methodInfo = api.class_get_methods(klass, &iter);
        if (methodInfo == nullptr) {
          break;
        }
        const char *methodName = api.method_get_name(methodInfo);
        if (!methodNameContainsAny(methodName, methodKeys, 7)) {
          continue;
        }
        uint32_t paramCount = api.method_get_param_count(methodInfo);
        LOGI("候选方法: %s::%s(%u)", klassName ? klassName : "<null>", methodName ? methodName : "<null>", paramCount);
        printed++;
        if (printed >= 120) {
          LOGI("候选方法日志已截断，避免刷屏");
          dumped = true;
          return;
        }
      }
    }
  }
  dumped = true;
}

int patchTargets(Il2CppApi &api) {
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
          if (!classMatch(klassName, klassNs, t.classKeyword)) {
            continue;
          }
          void *replace = resolveActionPtr(t.action);
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
  time_t startedAt = time(nullptr);
  bool loggedTargets = false;
  for (int i = 0; i < kMaxRetry; i++) {
    Il2CppApi api {};
    if (!resolveIl2cpp(api)) {
      if ((i % 10) == 0) {
        LOGI("waiting il2cpp... retry=%d", i);
      }
      sleep(kRetrySleepSec);
      continue;
    }

    logCandidateMethods(api);

    // Delay a bit after il2cpp appears to avoid crashing on early domain access.
    time_t now = time(nullptr);
    if ((now - startedAt) < gPatchDelaySec) {
      if ((i % 5) == 0) {
        LOGI("il2cpp ready, wait VM stabilize... elapsed=%llds/%ds", static_cast<long long>(now - startedAt),
             gPatchDelaySec);
      }
      sleep(kRetrySleepSec);
      continue;
    }

    int patchedNow = patchTargets(api);
    if (!loggedTargets || patchedNow > 0 || (i % 10) == 0) {
      int totalHit = 0;
      for (auto &t : gTargets) {
        totalHit += t.hitCount;
      }
      LOGI("补丁状态: 循环=%d 本轮新增=%d 累计命中=%d", i, patchedNow, totalHit);
      for (auto &t : gTargets) {
        LOGI("目标命中: 类=%s 方法=%s 次数=%d", t.classKeyword[0] ? t.classKeyword : "*", t.method,
             t.hitCount);
      }
      loggedTargets = true;
    }

    bool ready = true;
    for (auto &t : gTargets) {
      if (t.hitCount <= 0) {
        ready = false;
        break;
      }
    }
    if (ready) {
      LOGI("il2cpp绕过已生效: 所有目标都已完成补丁");
      return nullptr;
    }
    sleep(kRetrySleepSec);
  }

  LOGE("超时: 仍有目标未完成补丁");
  for (auto &t : gTargets) {
    LOGE("目标缺失: 类=%s 方法=%s 次数=%d", t.classKeyword[0] ? t.classKeyword : "*", t.method, t.hitCount);
  }
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
    proc_name_[0] = '\0';
    insn_[0] = '\0';
    app_data_dir_[0] = '\0';

    if (args != nullptr && env_ != nullptr) {
      jstrToBuf(env_, args->nice_name, proc_name_, sizeof(proc_name_));
      jstrToBuf(env_, args->instruction_set, insn_, sizeof(insn_));
      jstrToBuf(env_, args->app_data_dir, app_data_dir_, sizeof(app_data_dir_));

      bool byName = isTargetProcessName(proc_name_);
      bool byDir = isTargetByDataDir(app_data_dir_);
      target_ = byName || byDir;
      recordLastProcess(args->uid, proc_name_, insn_, app_data_dir_, target_);
      if (target_) {
        gPatchDelaySec = loadPatchDelayConfig();
      }
    }

    if (!target_) {
      api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }
  }

  void postAppSpecialize(const AppSpecializeArgs *args) override {
    (void)args;
    if (!target_) {
      return;
    }
    ensureLogFileReady();
    LOGI("start patching: name=%s dir=%s isa=%s delay=%ds", proc_name_[0] ? proc_name_ : "-",
         app_data_dir_[0] ? app_data_dir_ : "-", insn_[0] ? insn_ : "-", gPatchDelaySec);

    pthread_t t {};
    if (pthread_create(&t, nullptr, workerThread, nullptr) == 0) {
      pthread_detach(t);
    } else {
      LOGE("pthread_create failed");
    }
  }

  void preServerSpecialize(ServerSpecializeArgs *args) override {
    (void)args;
    api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
  }

 private:
  Api *api_ = nullptr;
  JNIEnv *env_ = nullptr;
  bool target_ = false;
  char proc_name_[192] = {0};
  char insn_[64] = {0};
  char app_data_dir_[320] = {0};
};

REGISTER_ZYGISK_MODULE(HsahcZygiskModule)
