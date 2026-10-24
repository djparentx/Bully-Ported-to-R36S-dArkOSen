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
static void crash_handler(int sig, siginfo_t *si, void *ucv)
{
  ucontext_t *uc = (ucontext_t *)ucv;
  uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
  uintptr_t lr = (uintptr_t)uc->uc_mcontext.regs[30];
  uintptr_t sp = (uintptr_t)uc->uc_mcontext.sp;
  uintptr_t base = (uintptr_t)text_virtbase;
  char buf[256];
  int n;

  fflush(stdout);

  n = snprintf(buf, sizeof(buf), "[CRASH] signal=%d fault_addr=0x%lx pc=0x%lx lr=0x%lx sp=0x%lx\n",
               sig, (unsigned long)si->si_addr, (unsigned long)pc, (unsigned long)lr, (unsigned long)sp);
  write(STDERR_FILENO, buf, n);

  if (base && pc >= base && pc < base + text_size)
  {
    n = snprintf(buf, sizeof(buf), "[CRASH] pc: libBully.so+0x%lx\n", (unsigned long)(pc - base));
    write(STDERR_FILENO, buf, n);
  }
  if (base && lr >= base && lr < base + text_size)
  {
    n = snprintf(buf, sizeof(buf), "[CRASH] lr: libBully.so+0x%lx\n", (unsigned long)(lr - base));
    write(STDERR_FILENO, buf, n);
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
