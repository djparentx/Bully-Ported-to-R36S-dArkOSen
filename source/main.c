/* main.c
 *
 * Copyright (C) 2026 givethesourceplox, fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdio.h>
#include <signal.h>
#include <ucontext.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "hooks.h"
#include "imports.h"
#include "zip_fs.h"
#include "setup.h"
#include "cpplib_loader.h"

static void check_data(void)
{
  struct stat st;

  // the installer puts this in place; missing means a damaged install
  if (stat("assets/data_0.zip", &st) < 0)
    fatal_error("Could not find\n%s.\nCheck your data files.", "assets/data_0.zip");
}

// run from the binary's own folder so relative paths (assets/, savegames/, config.txt) resolve
static void chdir_to_exe_dir(void)
{
  char path[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (len <= 0)
    return;
  path[len] = '\0';

  char *slash = strrchr(path, '/');
  if (!slash || slash == path)
    return;
  *slash = '\0';

  if (chdir(path) < 0)
    fatal_error("Could not enter game folder\n%s", path);
}

// Fatal signals: log where it happened (libBully.so offsets when inside the game), then die
static int crash_in_code(uintptr_t addr)
{
  uintptr_t game = (uintptr_t)text_virtbase, cpp = 0;
  size_t cpp_size = 0;

  cpplib_get_range(&cpp, &cpp_size);
  return (game && addr >= game && addr < game + text_size) ||
         (cpp && addr >= cpp && addr < cpp + cpp_size);
}

static void crash_addr(const char *what, uintptr_t addr)
{
  uintptr_t game = (uintptr_t)text_virtbase, cpp = 0;
  size_t cpp_size = 0;
  char buf[160];
  int n;

  cpplib_get_range(&cpp, &cpp_size);
  if (game && addr >= game && addr < game + text_size)
    n = snprintf(buf, sizeof(buf), "[CRASH] %-5s libBully.so+0x%lx\n", what, (unsigned long)(addr - game));
  else if (cpp && addr >= cpp && addr < cpp + cpp_size)
    n = snprintf(buf, sizeof(buf), "[CRASH] %-5s libc++_shared.so+0x%lx\n", what, (unsigned long)(addr - cpp));
  else
    n = snprintf(buf, sizeof(buf), "[CRASH] %-5s 0x%lx\n", what, (unsigned long)addr);
  write(STDERR_FILENO, buf, n);
}

// Fatal signals: where it happened, then return addresses found on the stack
// that point into the game's code (innermost first), then die
static void crash_handler(int sig, siginfo_t *si, void *ucv)
{
  ucontext_t *uc = (ucontext_t *)ucv;
  uintptr_t sp = (uintptr_t)uc->uc_mcontext.sp;
  const uintptr_t *stack = (const uintptr_t *)sp;
  char buf[160];
  int n, found = 0;

  fflush(stdout);

  n = snprintf(buf, sizeof(buf), "[CRASH] signal=%d fault_addr=0x%lx sp=0x%lx\n",
               sig, (unsigned long)si->si_addr, (unsigned long)sp);
  write(STDERR_FILENO, buf, n);
  crash_addr("pc", (uintptr_t)uc->uc_mcontext.pc);
  crash_addr("lr", (uintptr_t)uc->uc_mcontext.regs[30]);

  // 32 KB of stack; if this runs off the mapped stack the second fault is
  // fatal (SA_RESETHAND), so it can't loop
  for (int i = 0; i < 4096 && found < 16; i++)
  {
    if (crash_in_code(stack[i]))
    {
      crash_addr("stack", stack[i]);
      found++;
    }
  }

  raise(sig); // SA_RESETHAND: the default action now runs
}

static void install_crash_handler(void)
{
  static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sigemptyset(&sa.sa_mask);
  for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
    sigaction(sigs[i], &sa, NULL);
}

int main(void)
{
  chdir_to_exe_dir();

  // line-buffered so the log is complete up to a crash
  setvbuf(stdout, NULL, _IOLBF, 0);
  install_crash_handler();

  // the user's values (defaults for anything missing, clamped), then
  // config.txt is rewritten from the commented template with those values
  read_config(CONFIG_NAME);
  write_config(CONFIG_NAME);

  debugPrintf_setCompatDelayMs(0);

  // one window for the installer, the splash and the game
  if (!video_open())
    fatal_error("Could not open the display");

  if (!setup_is_installed() && !setup_run())
    port_exit(0);

  splash_show();

  zip_fs_init();

  check_data();

  // Load libc++_shared.so first — provides __ndk1 C++ stdlib symbols
  extern int cpplib_load(const char *filename);
  if (cpplib_load("libc++_shared.so") < 0)
    fatal_error("Could not load libc++_shared.so.\nCopy it from the APK\nlib/arm64-v8a/ folder\nto the game directory.");
  if (so_load(SO_NAME) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);

  // won't save without it
  mkdir("savegames", 0777);

  update_imports();

  so_relocate();
  so_resolve(dynlib_functions, dynlib_numfunctions, 1);

  patch_openal();
  // patch_opengl();
  patch_game();
  // patch_movie();

  // can't set it in the initializer because it's not constant
  stderr_fake = stderr;
  extern FILE *stdin_fake;
  stdin_fake = stdin;

  // Resolve all symbol addresses BEFORE so_finalize.
  // After svcMapProcessCodeMemory, the source heap pages become Perm_None
  // and so_find_addr (which reads syms/dynstrtab from text_base) will crash.
  // Use so_find_addr_rx to get the executable text_virtbase address.
  extern void jni_init(void);
  jni_init(); // sets up fake_vm and resolves JNI_OnLoad address

  // Set StorageRootPath directly — the game uses this for data file paths.
  // On Android, NvUtilInit sets this via JNI getAppLocalValue("STORAGE_ROOT").
  // We set it here to bypass the JNI roundtrip.
  strcpy((char *)so_find_addr("StorageRootPath"), ".");

  so_finalize();
  so_flush_caches();

  // GOT-based hooks: patch PLT/GOT entries now that data segment is RW.
  // Must be after so_finalize() (GOT accessible) and before jni_start() (game runs).
  patch_game_post_finalize();

  // Run C++ global constructors (.init_array).
  // This populates AllWarTypes, registers WarLangType objects, and performs
  // other critical static initialization that the game expects before main logic.
  so_execute_init_array();

  // Now call into the loaded .so - code is executable at text_virtbase
  extern void jni_start(void);
  jni_start();

  return 0;
}
