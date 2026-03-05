#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>

#include <errno.h>
#include <elf.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
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

constexpr int kMaxRetry = 180;
constexpr int kRetrySleepSec = 1;

int gLogFd = -1;
int gLastProcFd = -1;

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
  char line[2300] = {0};
  int n = snprintf(line, sizeof(line), "%s [%s] %s\n", t, level, message);
  if (n > 0) {
    writeLineFd(gLogFd, line);
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

#define LOGI(...) logPrint(ANDROID_LOG_INFO, "INFO", __VA_ARGS__)
#define LOGE(...) logPrint(ANDROID_LOG_ERROR, "ERROR", __VA_ARGS__)

bool jstrToBuf(JNIEnv *env, jstring js, char *out, size_t outSize) {
  if (out == nullptr || outSize == 0) {
    return false;
  }
  out[0] = '\0';
  if (env == nullptr || js == nullptr) {
    return false;
  }
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
  if (appDataDir == nullptr || appDataDir[0] == '\0') {
    return false;
  }
  return strstr(appDataDir, kTargetProcess) != nullptr;
}

void recordLastProcess(jint uid, const char *name, const char *isa, const char *appDataDir, bool target) {
  if ((name == nullptr || name[0] == '\0') && (appDataDir == nullptr || appDataDir[0] == '\0')) {
    return;
  }
  if (gLastProcFd < 0) {
    mkdir(kLogDir, 0755);
    gLastProcFd = open(kLastProcPath, O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
  }
  if (gLastProcFd < 0) {
    return;
  }

  char t[32] = {0};
  makeTimeStr(t, sizeof(t));

  const char *safeName = (name && name[0]) ? name : "-";
  const char *safeIsa = (isa && isa[0]) ? isa : "-";
  const char *safeDir = (appDataDir && appDataDir[0]) ? appDataDir : "-";
  char line[1200] = {0};
  int n = snprintf(line, sizeof(line), "%s uid=%d name=%s isa=%s dir=%s target=%s\n", t,
                   static_cast<int>(uid), safeName, safeIsa, safeDir, target ? "1" : "0");
  if (n > 0) {
    writeLineFd(gLastProcFd, line);
  }
}

void ensureLogFileReady() {
  if (gLogFd >= 0) {
    return;
  }

  // Prefer app private path first to avoid SELinux restriction when writing /data/adb from app context.
  gLogFd = open(kAppLogPath, O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
  if (gLogFd >= 0) {
    LOGI("log opened: %s", kAppLogPath);
    return;
  }

  mkdir(kLogDir, 0755);
  gLogFd = open(kAdbLogPath, O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
  if (gLogFd >= 0) {
    LOGI("log opened (fallback): %s", kAdbLogPath);
    return;
  }

  __android_log_print(ANDROID_LOG_ERROR, kLogTag, "open log failed errno=%d", errno);
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

TargetSpec gTargets[] = {
    {"IsVersionLessThanTargetVersion", "GameMgr", kReturnFalse, 0},
    {"VersionCompare", "GameMgr", kReturnZero, 0},
    {"ConfirmVersionForceUpdateJumpCallback", "GameMgr", kReturnVoid, 0},
    {"VersionForceUpdateJump", "GameMgr", kReturnVoid, 0},
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
  if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
      eh.e_shoff == 0 || eh.e_shentsize != sizeof(Elf64_Shdr) || eh.e_shnum == 0) {
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
  uintptr_t off = 0;
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
      if (strcmp(name, symName) != 0) {
        continue;
      }
      if (sym.st_value == 0) {
        continue;
      }
      off = static_cast<uintptr_t>(sym.st_value);
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
  *symOffsetOut = off;
  return true;
}

bool isExecutableAddress(void *addr) {
  if (addr == nullptr) {
    return false;
  }
  FILE *fp = fopen("/proc/self/maps", "r");
  if (fp == nullptr) {
    return false;
  }
  uintptr_t target = reinterpret_cast<uintptr_t>(addr);
  char line[512] = {0};
  bool exec = false;
  while (fgets(line, sizeof(line), fp) != nullptr) {
    unsigned long long start = 0;
    unsigned long long end = 0;
    char perms[8] = {0};
    if (sscanf(line, "%llx-%llx %7s", &start, &end, perms) != 3) {
      continue;
    }
    if (target >= start && target < end) {
      exec = (strchr(perms, 'x') != nullptr);
      break;
    }
  }
  fclose(fp);
  return exec;
}

bool patchCodeArm64(void *func, int action) {
#if defined(__aarch64__)
  if (func == nullptr) {
    return false;
  }
  if (!makeWritable(func)) {
    return false;
  }
  auto *p = reinterpret_cast<uint32_t *>(func);
  if (action == kReturnFalse || action == kReturnZero) {
    p[0] = 0x52800000U;  // mov w0, #0
    p[1] = 0xD65F03C0U;  // ret
    __builtin___clear_cache(reinterpret_cast<char *>(p), reinterpret_cast<char *>(p + 2));
    return true;
  }
  if (action == kReturnVoid) {
    p[0] = 0xD65F03C0U;  // ret
    __builtin___clear_cache(reinterpret_cast<char *>(p), reinterpret_cast<char *>(p + 1));
    return true;
  }
#else
  (void)func;
  (void)action;
#endif
  return false;
}

bool patchMethodPointer(const void *methodInfo, int action, void *replace, const char *klass, const char *method,
                        uint32_t paramCount) {
  auto *m = reinterpret_cast<MethodInfoPatch *>(const_cast<void *>(methodInfo));
  if (m == nullptr || m->methodPointer == nullptr) {
    return false;
  }

  void *entry = m->methodPointer;
  if (isExecutableAddress(entry) && patchCodeArm64(entry, action)) {
    LOGI("patched-code %s::%s(%u) entry=%p", klass, method, paramCount, entry);
    return true;
  }

  if (replace == nullptr || m->methodPointer == replace) {
    return false;
  }
  if (!makeWritable(m)) {
    LOGE("mprotect failed: %s::%s", klass, method);
    return false;
  }

  void *old = m->methodPointer;
  m->methodPointer = replace;
  __builtin___clear_cache(reinterpret_cast<char *>(m), reinterpret_cast<char *>(m + 1));
  LOGI("patched-meta %s::%s(%u) old=%p new=%p", klass, method, paramCount, old, replace);
  return true;
}

bool resolveIl2cpp(Il2CppApi &api) {
  char realPath[512] = {0};
  uintptr_t libBase = 0;
  bool hasRealPath = false;
  void *hRealNoLoad = nullptr;
  void *hRealLoad = nullptr;
  void *hShortNoLoad = nullptr;
  void *hShortLoad = nullptr;
  if (findLoadedSoPath(kIl2cppSo, realPath, sizeof(realPath))) {
    hasRealPath = true;
    LOGI("found il2cpp in maps: %s", realPath);
    int ok = access(realPath, R_OK);
    LOGI("il2cpp path access=%d errno=%d", ok, ok == 0 ? 0 : errno);
    if (findLibraryBase(realPath, &libBase)) {
      LOGI("il2cpp base=%p", reinterpret_cast<void *>(libBase));
    }
    hRealNoLoad = dlopen(realPath, RTLD_NOW | RTLD_NOLOAD);
    hRealLoad = dlopen(realPath, RTLD_NOW);
  }
  hShortNoLoad = dlopen(kIl2cppSo, RTLD_NOW | RTLD_NOLOAD);
  hShortLoad = dlopen(kIl2cppSo, RTLD_NOW);

  auto resolveFn = [&](const char *sym, void **out) -> bool {
    void *p = dlsym(RTLD_DEFAULT, sym);
    if (p != nullptr) {
      memcpy(out, &p, sizeof(p));
      return true;
    }
    if (hRealNoLoad != nullptr) {
      p = dlsym(hRealNoLoad, sym);
      if (p != nullptr) {
        memcpy(out, &p, sizeof(p));
        return true;
      }
    }
    if (hRealLoad != nullptr) {
      p = dlsym(hRealLoad, sym);
      if (p != nullptr) {
        memcpy(out, &p, sizeof(p));
        return true;
      }
    }
    if (hShortNoLoad != nullptr) {
      p = dlsym(hShortNoLoad, sym);
      if (p != nullptr) {
        memcpy(out, &p, sizeof(p));
        return true;
      }
    }
    if (hShortLoad != nullptr) {
      p = dlsym(hShortLoad, sym);
      if (p != nullptr) {
        memcpy(out, &p, sizeof(p));
        return true;
      }
    }
    if (hasRealPath && libBase != 0) {
      uintptr_t off = 0;
      if (resolveElfSymOffset64(realPath, sym, &off) && off != 0) {
        p = reinterpret_cast<void *>(libBase + off);
        memcpy(out, &p, sizeof(p));
        LOGI("resolved by ELF: %s off=0x%zx addr=%p", sym, static_cast<size_t>(off), p);
        return true;
      }
    }
    LOGE("missing il2cpp export: %s", sym);
    return false;
  };

  bool allHandleNull = (hRealNoLoad == nullptr && hRealLoad == nullptr && hShortNoLoad == nullptr &&
                        hShortLoad == nullptr);
  if (allHandleNull) {
    const char *err = dlerror();
    LOGE("dlopen il2cpp handles all null: %s", err ? err : "<no dlerror>");
    // Do not return here. RTLD_DEFAULT may still resolve symbols in current linker namespace.
  }

  if (!resolveFn("il2cpp_domain_get", reinterpret_cast<void **>(&api.domain_get))) return false;
  if (!resolveFn("il2cpp_thread_attach", reinterpret_cast<void **>(&api.thread_attach))) return false;
  if (!resolveFn("il2cpp_domain_get_assemblies", reinterpret_cast<void **>(&api.domain_get_assemblies))) return false;
  if (!resolveFn("il2cpp_assembly_get_image", reinterpret_cast<void **>(&api.assembly_get_image))) return false;
  if (!resolveFn("il2cpp_image_get_name", reinterpret_cast<void **>(&api.image_get_name))) return false;
  if (!resolveFn("il2cpp_image_get_class_count", reinterpret_cast<void **>(&api.image_get_class_count))) return false;
  if (!resolveFn("il2cpp_image_get_class", reinterpret_cast<void **>(&api.image_get_class))) return false;
  if (!resolveFn("il2cpp_class_get_name", reinterpret_cast<void **>(&api.class_get_name))) return false;
  if (!resolveFn("il2cpp_class_get_namespace", reinterpret_cast<void **>(&api.class_get_namespace))) return false;
  if (!resolveFn("il2cpp_class_get_methods", reinterpret_cast<void **>(&api.class_get_methods))) return false;
  if (!resolveFn("il2cpp_method_get_name", reinterpret_cast<void **>(&api.method_get_name))) return false;
  if (!resolveFn("il2cpp_method_get_param_count", reinterpret_cast<void **>(&api.method_get_param_count)))
    return false;

  LOGI("il2cpp symbols resolved");
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
  int foundByName = 0;
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
          foundByName++;
          if (strictClass && !classMatch(klassName, klassNs, t.classKeyword)) {
            continue;
          }
          void *replace = resolveActionPtr(t.action);
          if (patchMethodPointer(methodInfo, t.action, replace, klassName ? klassName : "<null>", methodName,
                                 paramCount)) {
            t.hitCount++;
            patched++;
          }
        }
      }
    }
  }
  if (foundByName > 0 && patched == 0) {
    LOGI("found target method names=%d but patch=0 (strict=%d)", foundByName, strictClass ? 1 : 0);
  }
  return patched;
}

void *workerThread(void *) {
  time_t startedAt = time(nullptr);
  for (int i = 0; i < kMaxRetry; i++) {
    Il2CppApi api{};
    if (!resolveIl2cpp(api)) {
      if ((i % 10) == 0) {
        LOGI("waiting il2cpp... retry=%d", i);
      }
      sleep(kRetrySleepSec);
      continue;
    }

    // il2cpp symbols may be visible before VM is stable. Delay patching to reduce startup crash risk.
    time_t now = time(nullptr);
    if ((now - startedAt) < 12) {
      if ((i % 5) == 0) {
        LOGI("il2cpp ready but waiting VM stabilize... elapsed=%llds", static_cast<long long>(now - startedAt));
      }
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
      LOGI("il2cpp bypass active: total=%d strict=%d fallback=%d", total, strictPatched, fallbackPatched);
      return nullptr;
    }

    sleep(kRetrySleepSec);
  }

  LOGE("timeout: failed to patch il2cpp targets");
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
    }

    if (!target_) {
      api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
      return;
    }

    __android_log_print(ANDROID_LOG_INFO, kLogTag, "target process matched: name=%s dir=%s",
                        proc_name_[0] ? proc_name_ : "-", app_data_dir_[0] ? app_data_dir_ : "-");
  }

  void postAppSpecialize(const AppSpecializeArgs *args) override {
    (void)args;
    if (!target_) {
      return;
    }

    ensureLogFileReady();
    LOGI("start patching: name=%s dir=%s isa=%s", proc_name_[0] ? proc_name_ : "-",
         app_data_dir_[0] ? app_data_dir_ : "-", insn_[0] ? insn_ : "-");

    pthread_t t{};
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
