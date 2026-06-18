#include "jank_runtime.h"
#include "jank_node.h"
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string_name.hpp>

// GC_THREADS + <gc.h> MUST precede jank/error.hpp: the latter pulls in immer's gc_heap, which
//   #errors ("requires libgc") unless libgc is already in scope, and GC_register_my_thread/GC_allow_register_threads
//   are gated on GC_THREADS
#define GC_THREADS
#include <gc.h>
#include <jank/c_api.h>
#include <jank/error.hpp>
#include <jank/gc.hpp>                  // UseGC (GC placement-new), for __rt_ctx
#include <jank/runtime/context.hpp>     // jank::runtime::context + __rt_ctx
#include <jank/runtime/obj/persistent_array_map.hpp> // gc_descriptor (ns maps)
#include <jank/runtime/obj/persistent_string.hpp>    // gc_descriptor (strings)
#include <jank/runtime/detail/native_array_map.hpp>
#include <llvm/Support/TargetSelect.h>  // InitializeNativeTarget*
#include <locale>
#include <clocale>
#include <fstream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <queue>
#include <functional>
#include <future>
#include <atomic>
#include <utility>
#include <filesystem>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <csignal>

extern "C" void jank_load_clojure_core_native();
extern "C" void jank_load_clojure_core();
// phase-2 load-fns the nREPL server transitively needs (names: jank_load_<munged ns>)
extern "C" void jank_load_clojure_string();
extern "C" void jank_load_clojure_walk();
extern "C" void jank_load_jank_compiler_native();
extern "C" void jank_load_jank_nrepl_server_core();
extern "C" void jank_load_jank_nrepl_server_inspect();
extern "C" void jank_load_jank_nrepl_server_handler();
extern "C" void jank_load_jank_nrepl_server_bencode();
extern "C" void jank_load_jank_nrepl_server_capture();
extern "C" void jank_load_jank_nrepl_server_util();
extern "C" void jank_load_jank_nrepl_server_eval();
extern "C" void jank_load_jank_nrepl_server_parsec();
extern "C" void jank_load_jank_nrepl_server_handler_close();
extern "C" void jank_load_jank_nrepl_server_handler_clone();
extern "C" void jank_load_jank_nrepl_server_handler_describe();
extern "C" void jank_load_jank_nrepl_server_handler_completions();
extern "C" void jank_load_jank_nrepl_server_handler_eval();
extern "C" void jank_load_jank_nrepl_server_handler_lookup();

using namespace godot;

// ===== THE BRIDGE (jank -> our C++ -> Godot) ==========================
// jank's JIT can't resolve our dylib symbols directly, so jank reaches these via cpp/raw
//   trampolines that reinterpret_cast an address we inject (see setup_godot_ns)
// `node` is the Godot Object*, passed through jank as an i64. WRITES run directly when we're
//   already on the main thread (the game loop), synchronous, no frame lag and fallback to
//   call_deferred when called from another thread (e.g. the REPL). READS only make sense on the
//   main thread and are always direct ie: synchronous
extern "C" {
    void gd_set_num(long node, char const *prop, double v);
    void gd_set_vec2(long node, char const *prop, double x, double y);
    void gd_set_color(long node, char const *prop, double r, double g, double b, double a);
    void gd_call0(long node, char const *method);
    void gd_call_num(long node, char const *method, double v);
    double gd_get_num(long node, char const *prop);   // read a scalar property
    double gd_get_vec2x(long node, char const *prop);  // read .x of a Vector2 property
    double gd_get_vec2y(long node, char const *prop);  // read .y of a Vector2 property
}

// No-op module loader for `godot`: the bridge ns is loaded eagerly in setup_godot_ns,
//   so this never actually runs - it just gives jank_module_register a valid load-fn so
//   that (require 'godot) in user scripts short-circuits as already-loaded.
extern "C" void jank_load_godot_noop() {}

namespace {

// ---- VM state ----
// jank runs on Godot's MAIN thread
// Our code (game loop, script loads, JankRuntime::eval) all runs there
// The REPL is jank's nREPL, its client handlers run on jank's own future threads, so nREPL-thread
//   WRITES go through call_deferred (is_main_thread() == false) while the game loop's reads+writes
//   are direct
std::thread::id g_main_id{};                  // captured at init, jank lives here
std::atomic<bool> g_vm_ready{ false };
std::atomic<bool> g_signal_quit{ false };     // set by SIGINT/SIGTERM handler

extern "C" void jank_godot_on_term_signal(int) { g_signal_quit.store(true); }

bool is_main_thread() { return std::this_thread::get_id() == g_main_id; }

// ======================================================================
// path resolver (replaces the hardcoded PCH / argv0 / install path)
// ======================================================================
namespace fs = std::filesystem;

bool dir_exists(std::string const &p) {
  std::error_code ec;
  return !p.empty() && fs::is_directory(p, ec);
}

bool file_exists(std::string const &p) {
  std::error_code ec;
  return !p.empty() && fs::is_regular_file(p, ec);
}

// User home directory: HOME on unix, USERPROFILE on Windows.
std::string home_dir() {
  if (char const *h{ ::getenv("HOME") }) { return h; }
  if (char const *u{ ::getenv("USERPROFILE") }) { return u; }
  return {};
}

std::string run_capture(std::string const &cmd) {
  std::string out;
#ifdef _WIN32
  FILE *p{ ::_popen(cmd.c_str(), "r") };
#else
  FILE *p{ ::popen(cmd.c_str(), "r") };
#endif
  if (p) {
    char b[512];
    std::size_t n;
    while ((n = ::fread(b, 1, sizeof(b), p)) > 0) { out.append(b, n); }
#ifdef _WIN32
    ::_pclose(p);
#else
    ::pclose(p);
#endif
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '\t')) {
    out.pop_back();
  }
  return out;
}

std::string setting_str(char const *key) {
  ProjectSettings *ps{ ProjectSettings::get_singleton() };
  if (ps && ps->has_setting(key)) {
    return std::string(String(ps->get_setting(key)).utf8().get_data());
  }
  return {};
}

std::string resolve_jank_home() {
  if (char const *e{ ::getenv("JANK_HOME") }) {
    if (dir_exists(e)) { return e; }
  }
  std::string const s{ setting_str("jank/jank_home") };
  if (dir_exists(s)) { return s; }
  std::string const brew{ run_capture("brew --prefix jank 2>/dev/null") };
  if (dir_exists(brew)) { return brew; }
  for (char const *c : { "/opt/homebrew/opt/jank", "/usr/local/opt/jank" }) {
    if (dir_exists(c)) { return c; }
  }
  return {};
}

std::string resolve_pch(std::string const &argv0) {
  // Explicit override wins (mirrors JANK_HOME; for non-standard cache locations).
  if (char const *e{ ::getenv("JANK_PCH") }) {
    if (file_exists(e)) { return e; }
  }
  // jank caches the PCH at <cache>/jank/<target-hash>/incremental.pch. Cache base:
  //   XDG_CACHE_HOME if set, else %LOCALAPPDATA% on Windows, else ~/.cache.
  auto cache_base = []() -> fs::path {
    if (char const *x{ ::getenv("XDG_CACHE_HOME") }) {
      if (*x) { return fs::path(x) / "jank"; }
    }
#ifdef _WIN32
    if (char const *l{ ::getenv("LOCALAPPDATA") }) {
      if (*l) { return fs::path(l) / "jank"; }
    }
#endif
    std::string const h{ home_dir() };
    return h.empty() ? fs::path{} : fs::path(h) / ".cache" / "jank";
  };
  // Newest <base>/<hash>/incremental.pch (more than one target can be cached).
  auto newest = [&]() -> std::string {
    std::string best;
    fs::file_time_type bestm{};
    std::error_code ec;
    fs::path const base{ cache_base() };
    if (base.empty() || !fs::is_directory(base, ec)) { return best; }
    for (auto const &entry : fs::directory_iterator(base, ec)) {
      if (ec) { break; }
      fs::path const pch{ entry.path() / "incremental.pch" };
      std::error_code fec;
      if (fs::is_regular_file(pch, fec)) {
        auto const m{ fs::last_write_time(pch, fec) };
        if (!fec && (best.empty() || m >= bestm)) { bestm = m; best = pch.string(); }
      }
    }
    return best;
  };
  std::string p{ newest() };
  if (!p.empty()) { return p; }
  if (file_exists(argv0)) {
    UtilityFunctions::print("jank: no PCH cached - running `jank check-health` to "
                            "build it (one-time, ~1 min)...");
#ifdef _WIN32
    run_capture("\"" + argv0 + "\" check-health >NUL 2>&1");
#else
    run_capture("'" + argv0 + "' check-health >/dev/null 2>&1");
#endif
    p = newest();
  }
  return p;
}

// ======================================================================
// init_runtime - jank_init_with_pch's body, replicated MINUS the local llvm::llvm_shutdown_obj (which would tear
//   the JIT down on return)
// Runs on the MAIN thread, afterwards jank_eval works there directly
// Jeaye (jank maintainer) suggested this as an interim embedding path until jank ships an official init/teardown C API
// Source: [jank c_api.cpp @308a857](https://github.com/jank-lang/jank/blob/308a8570c8995b7c2c9429a29fbe1b34c5941040/compiler%2Bruntime/src/cpp/jank/c_api.cpp#L1035)
// ======================================================================
bool init_runtime() {
  std::string const home{ resolve_jank_home() };
  if (home.empty()) {
    UtilityFunctions::printerr(
        "jank: could not locate a jank install. Install it (brew install jank) or "
        "set JANK_HOME / the `jank/jank_home` project setting");
    return false;
  }
  fs::path jbin{ fs::path(home) / "bin" /
#ifdef _WIN32
                 "jank.exe"
#else
                 "jank"
#endif
  };
  std::error_code canon_ec;
  fs::path const canon{ fs::weakly_canonical(jbin, canon_ec) };
  std::string argv0{ (canon_ec ? jbin : canon).string() };
  std::string const pch{ resolve_pch(argv0) };
  if (pch.empty()) {
    UtilityFunctions::printerr("jank: could not find or build the PCH "
                               "(~/.cache/jank/*/incremental.pch). Run `",
                               String(argv0.c_str()), " check-health` once.");
    return false;
  }
  std::ifstream in{ pch, std::ios::binary | std::ios::ate };
  std::streamsize const size{ in.tellg() };
  in.seekg(0);
  static std::vector<char> buf(static_cast<std::size_t>(size));
  in.read(buf.data(), size);

  // Locale: env locale, but LC_NUMERIC=C so "3.14" parses regardless of locale.
  try { std::locale::global(std::locale("")); }
  catch (...) { /* misconfigured env locale (can throw on Windows) - keep default */ }
  std::setlocale(LC_NUMERIC, "C");

  GC_set_all_interior_pointers(1);
  GC_init();
  GC_allow_register_threads();

  // GC type descriptors for the ns array-maps + strings. WITHOUT these, strings are mis-traced (garbled output)
  //   and ns maps corrupt (vars vanish)
  {
    GC_word bm[GC_BITMAP_SIZE(jank::runtime::obj::persistent_array_map)]{ 0 };
    GC_set_bit(bm,
               GC_OFFSETOF_IN_PTRS(jank::runtime::obj::persistent_array_map, data)
                 + GC_OFFSETOF_IN_PTRS(jank::runtime::detail::native_array_map, data));
    GC_set_bit(bm, GC_OFFSETOF_IN_PTRS(jank::runtime::obj::persistent_array_map, meta));
    jank::runtime::obj::persistent_array_map::gc_descriptor = GC_make_descriptor(
        bm, GC_SIZEOF_IN_PTRS(jank::runtime::obj::persistent_array_map));
  }
  {
    GC_word bm[GC_BITMAP_SIZE(jank::runtime::obj::persistent_string)]{ 0 };
    GC_set_bit(bm, GC_OFFSETOF_IN_PTRS(jank::runtime::obj::persistent_string, data));
    jank::runtime::obj::persistent_string::gc_descriptor = GC_make_descriptor(
        bm, GC_SIZEOF_IN_PTRS(jank::runtime::obj::persistent_string));
  }

  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmParser();
  llvm::InitializeNativeTargetAsmPrinter();
  jank_resource_register("incremental.pch", buf.data(),
                         static_cast<jank_usize>(size));
  jank::runtime::__rt_ctx = new (UseGC) jank::runtime::context{};
  jank_load_clojure_core_native();
  jank_load_clojure_core();
  jank_module_register("clojure.core", &jank_load_clojure_core);
  jank_module_set_loaded("clojure.core");

  UtilityFunctions::print("jank: runtime up on the MAIN thread (home=",
                          String(home.c_str()), ")");
  return true;
}

// Run a jank job on the main thread. All callers (game loop, script loads, JankRuntime::eval)
//   already run on the main thread, so this is a direct inline call
// (nREPL handlers eval on jank's own threads and don't route through here)
// NOTE: Kept here in case I need it, may remove in the future
template <typename F>
auto run_on_main(F &&f) -> decltype(f()) {
  return f();
}

// THE ERROR BOUNDARY
// jank reports failures by THROWING jtl::ref<jank::error::base>
// Catch it at the eval site so a bad form is logged + ignored, not fatal
// (Critical: godot-cpp frames are -fno-exceptions, so an escaping jank throw would terminate Godot)
void safe_eval(std::string const &code, char const *ctx) {
  try {
    jank_eval(jank_read_string_c(code.c_str()));
  } catch (jank::error_ref const &e) {
    UtilityFunctions::printerr("jank: ", ctx, " - [",
                               jank::error::kind_str(e->kind), "] ",
                               String(jtl::immutable_string(e->message).c_str()),
                               "  (form ignored)");
  } catch (...) {
    UtilityFunctions::printerr("jank: ", ctx,
                               " - non-jank error; form ignored: ",
                               String(code.substr(0, 120).c_str()));
  }
}

void eval_on_jank(std::string const &code) {
  run_on_main([code] { safe_eval(code, "repl"); });
}

// ---- the `godot` bridge namespace: trampolines + helpers jank scripts call ----
// Each cpp/raw symbol must be DEFINED in a separate eval from its USE (jank analyzes a (do (cpp/raw …) (cpp/use …))
//   before the raw registers the symbol)
void setup_godot_ns() {
  auto ev = [](char const *s) { jank_eval(jank_read_string_c(s)); };
  ev("(in-ns 'godot)");
  ev("(clojure.core/refer 'clojure.core)");

  auto bind = [](char const *name, void *fp) {
    jank_var_bind_root(jank_var_intern_c("godot", name),
                       jank_integer_create(reinterpret_cast<int64_t>(fp)));
  };
  bind("set-num-fp", reinterpret_cast<void *>(&gd_set_num));
  bind("set-vec2-fp", reinterpret_cast<void *>(&gd_set_vec2));
  bind("set-color-fp", reinterpret_cast<void *>(&gd_set_color));
  bind("call0-fp", reinterpret_cast<void *>(&gd_call0));
  bind("call-num-fp", reinterpret_cast<void *>(&gd_call_num));
  bind("get-num-fp", reinterpret_cast<void *>(&gd_get_num));
  bind("get-vec2x-fp", reinterpret_cast<void *>(&gd_get_vec2x));
  bind("get-vec2y-fp", reinterpret_cast<void *>(&gd_get_vec2y));

  // Trampolines (jank-compiled, reinterpret the address + call through it)
  ev(R"JANK((cpp/raw "extern \"C\" void jank_t_set_num(long fp,long n,const char* p,double v){ reinterpret_cast<void(*)(long,const char*,double)>(fp)(n,p,v); }"))JANK");
  ev(R"JANK((cpp/raw "extern \"C\" void jank_t_set_vec2(long fp,long n,const char* p,double x,double y){ reinterpret_cast<void(*)(long,const char*,double,double)>(fp)(n,p,x,y); }"))JANK");
  ev(R"JANK((cpp/raw "extern \"C\" void jank_t_set_color(long fp,long n,const char* p,double r,double g,double b,double a){ reinterpret_cast<void(*)(long,const char*,double,double,double,double)>(fp)(n,p,r,g,b,a); }"))JANK");
  ev(R"JANK((cpp/raw "extern \"C\" void jank_t_call0(long fp,long n,const char* m){ reinterpret_cast<void(*)(long,const char*)>(fp)(n,m); }"))JANK");
  ev(R"JANK((cpp/raw "extern \"C\" void jank_t_call_num(long fp,long n,const char* m,double v){ reinterpret_cast<void(*)(long,const char*,double)>(fp)(n,m,v); }"))JANK");
  ev(R"JANK((cpp/raw "extern \"C\" double jank_t_get_num(long fp,long n,const char* p){ return reinterpret_cast<double(*)(long,const char*)>(fp)(n,p); }"))JANK");
  ev(R"JANK((cpp/raw "extern \"C\" double jank_t_get_vec2x(long fp,long n,const char* p){ return reinterpret_cast<double(*)(long,const char*)>(fp)(n,p); }"))JANK");
  ev(R"JANK((cpp/raw "extern \"C\" double jank_t_get_vec2y(long fp,long n,const char* p){ return reinterpret_cast<double(*)(long,const char*)>(fp)(n,p); }"))JANK");

  // Public helpers + wrappers now live in res://godot.jank, readable jank source the IDE can resolve
  // The address-binding + trampolines above stay in C++ (addresses are runtime-only, trampolines must
  //   be registered before this file uses them)
  // NOTE: jank_read_string_c reads ONE form, so wrap the multi-form file in (do …)
  {
    Ref<FileAccess> f{ FileAccess::open("res://godot.jank", FileAccess::READ) };
    if (f.is_valid()) {
      std::string const body{ f->get_as_text().utf8().get_data() };
      jank_eval(jank_read_string_c(("(do\n" + body + "\n)").c_str()));
    } else {
      UtilityFunctions::printerr("jank: res://godot.jank not found - bridge helpers missing");
    }
  }
  // Mark `godot` as a loaded module so user scripts can (:require [godot]), a satisfied
  //   no-op at runtime that gives the IDE a real namespace to resolve godot/* against
  jank_module_register("godot", &jank_load_godot_noop);
  jank_module_set_loaded("godot");
  UtilityFunctions::print("jank: godot bridge ns ready (loaded res://godot.jank)");
}

// Boot the VM once, on the main thread
// Idempotent (guards re-entry / double-init)
bool ensure_vm() {
  if (g_vm_ready.load()) { return true; }
  if (!is_main_thread() && g_main_id != std::thread::id{}) {
    UtilityFunctions::printerr("jank: ensure_vm called off the main thread; ignored");
    return false;
  }
  g_main_id = std::this_thread::get_id();
  if (!init_runtime()) { return false; }
  setup_godot_ns();
  g_vm_ready.store(true);
  return true;
}

// Load a user .jank file into a namespace: default no-op ready/process keep the vars bound, then
//   the user body (may redefine), runs on the main thread
void load_script_into_ns(std::string const &ns, std::string const &body) {
  run_on_main([ns, body] {
    try {
      jank_eval(jank_read_string_c(("(in-ns '" + ns + ")").c_str()));
      jank_eval(jank_read_string_c("(clojure.core/refer 'clojure.core)"));
      jank_eval(jank_read_string_c("(clojure.core/defn ready [self] nil)"));
      jank_eval(jank_read_string_c("(clojure.core/defn process [self delta] nil)"));
    } catch (...) {
      UtilityFunctions::printerr("jank: ns '", String(ns.c_str()), "' setup failed");
      return;
    }
    safe_eval("(do\n" + body + "\n)", "jank-node script");
  });
}

// Call (ns/fn self [delta]) on the main thread (inline from the game loop)
void call_node_fn(std::string const &ns, char const *fn, int64_t self,
                  bool with_delta, double delta) {
  run_on_main([&] {
    try {
      jank_object_ref const v{ jank_var_intern_c(ns.c_str(), fn) };
      jank_object_ref const f{ jank_deref(v) };
      jank_object_ref const s{ jank_integer_create(self) };
      if (with_delta) {
        jank_call2(f, s, jank_real_create(delta));
      } else {
        jank_call1(f, s);
      }
    } catch (jank::error_ref const &e) {
      UtilityFunctions::printerr("jank: (", String(ns.c_str()), "/", fn, " self) - [",
                                 jank::error::kind_str(e->kind), "] ",
                                 String(jtl::immutable_string(e->message).c_str()));
    } catch (std::exception const &e) {
      UtilityFunctions::printerr("jank: (", String(ns.c_str()), "/", fn,
                                 " self) - C++ exception: ", e.what());
    } catch (...) {
      UtilityFunctions::printerr("jank: (", String(ns.c_str()), "/", fn,
                                 " self) - unknown C++ exception");
    }
  });
}

void start_nrepl() {
  jank_module_register("jank.compiler-native", &jank_load_jank_compiler_native);
  jank_module_register("clojure.string", &jank_load_clojure_string);
  jank_module_register("clojure.walk", &jank_load_clojure_walk);
  jank_module_register("jank.nrepl.server.core", &jank_load_jank_nrepl_server_core);
  jank_module_register("jank.nrepl.server.inspect", &jank_load_jank_nrepl_server_inspect);
  jank_module_register("jank.nrepl.server.handler", &jank_load_jank_nrepl_server_handler);
  jank_module_register("jank.nrepl.server.bencode", &jank_load_jank_nrepl_server_bencode);
  jank_module_register("jank.nrepl.server.capture", &jank_load_jank_nrepl_server_capture);
  jank_module_register("jank.nrepl.server.util", &jank_load_jank_nrepl_server_util);
  jank_module_register("jank.nrepl.server.eval", &jank_load_jank_nrepl_server_eval);
  jank_module_register("jank.nrepl.server.parsec", &jank_load_jank_nrepl_server_parsec);
  jank_module_register("jank.nrepl.server.handler.close", &jank_load_jank_nrepl_server_handler_close);
  jank_module_register("jank.nrepl.server.handler.clone", &jank_load_jank_nrepl_server_handler_clone);
  jank_module_register("jank.nrepl.server.handler.describe", &jank_load_jank_nrepl_server_handler_describe);
  jank_module_register("jank.nrepl.server.handler.completions", &jank_load_jank_nrepl_server_handler_completions);
  jank_module_register("jank.nrepl.server.handler.eval", &jank_load_jank_nrepl_server_handler_eval);
  jank_module_register("jank.nrepl.server.handler.lookup", &jank_load_jank_nrepl_server_handler_lookup);
  // nREPL handlers run on jank's own future threads (not main). REPL writes go through call_deferred
  //   (safe), the live-coding loop still works because a redefined fn runs in the game loop on the
  //   main thread, where its reads are synchronous
  // The server writes .nrepl-port for editors to connect
  try {
    jank_eval(jank_read_string_c("(require 'jank.nrepl.server.core)"));
    jank_eval(jank_read_string_c("(jank.nrepl.server.core/background-main)"));
    UtilityFunctions::print("jank: nREPL up - connect your editor via .nrepl-port");
  } catch (...) {
    UtilityFunctions::printerr("jank: nREPL failed to start (continuing without it)");
  }
}

// Clean shutdown on a normal quit: remove the .nrepl-port file
// nREPL's threads + the rest are reclaimed by the OS at process exit
void shutdown_jank() {
  static std::atomic<bool> done{ false };
  if (done.exchange(true)) { return; }
  UtilityFunctions::print("jank: clean shutdown");
  std::error_code ec;
  fs::remove(".nrepl-port", ec);
}

void install_signal_bridge() {
  ::signal(SIGINT, jank_godot_on_term_signal);
  ::signal(SIGTERM, jank_godot_on_term_signal);
}
bool repl_enabled() {
  ProjectSettings *ps{ ProjectSettings::get_singleton() };
  if (ps && ps->has_setting("jank/repl_enabled")) {
    return static_cast<bool>(ps->get_setting("jank/repl_enabled"));
  }
  return true; // default on (dev), set jank/repl_enabled=false for release
}

} // namespace

// ---- bridge definitions (file scope, exported; addresses injected into jank) ----
#define JANK_GD_OBJ(node) auto *o = reinterpret_cast<Object *>(node); if (!o) return
extern "C" __attribute__((visibility("default")))
void gd_set_num(long node, char const *prop, double v) {
  JANK_GD_OBJ(node);
  if (is_main_thread()) { o->set(String(prop), v); }
  else { o->call_deferred("set", String(prop), v); }
}

extern "C" __attribute__((visibility("default")))
void gd_set_vec2(long node, char const *prop, double x, double y) {
  JANK_GD_OBJ(node);
  Vector2 const val(static_cast<float>(x), static_cast<float>(y));
  if (is_main_thread()) { o->set(String(prop), val); }
  else { o->call_deferred("set", String(prop), val); }
}

extern "C" __attribute__((visibility("default")))
void gd_set_color(long node, char const *prop, double r, double g, double b, double a) {
  JANK_GD_OBJ(node);
  Color const val(static_cast<float>(r), static_cast<float>(g),
                  static_cast<float>(b), static_cast<float>(a));
  if (is_main_thread()) { o->set(String(prop), val); }
  else { o->call_deferred("set", String(prop), val); }
}

extern "C" __attribute__((visibility("default")))
void gd_call0(long node, char const *method) {
  JANK_GD_OBJ(node);
  if (is_main_thread()) { o->call(StringName(method)); }
  else { o->call_deferred(StringName(method)); }
}

extern "C" __attribute__((visibility("default")))
void gd_call_num(long node, char const *method, double v) {
  JANK_GD_OBJ(node);
  if (is_main_thread()) { o->call(StringName(method), v); }
  else { o->call_deferred(StringName(method), v); }
}
#undef JANK_GD_OBJ

// Reads: only valid on the main thread (the game loop); always direct/synchronous.
extern "C" __attribute__((visibility("default")))
double gd_get_num(long node, char const *prop) {
  auto *o = reinterpret_cast<Object *>(node);
  return o ? static_cast<double>(o->get(String(prop))) : 0.0;
}

extern "C" __attribute__((visibility("default")))
double gd_get_vec2x(long node, char const *prop) {
  auto *o = reinterpret_cast<Object *>(node);
  return o ? static_cast<Vector2>(o->get(String(prop))).x : 0.0;
}

extern "C" __attribute__((visibility("default")))
double gd_get_vec2y(long node, char const *prop) {
  auto *o = reinterpret_cast<Object *>(node);
  return o ? static_cast<Vector2>(o->get(String(prop))).y : 0.0;
}

// ======================================================================
// JankRuntime - optional autoload host
//   allows no hitch VM boot and optionally run nREPL
// ======================================================================
void JankRuntime::_bind_methods() {
  ClassDB::bind_method(D_METHOD("eval", "code"), &JankRuntime::eval);
}

void JankRuntime::_ready() {
  if (Engine::get_singleton()->is_editor_hint()) { return; }
  install_signal_bridge();
  if (!ensure_vm()) { return; }

  if (ProjectSettings::get_singleton()->has_setting("jank/main")) {
    String const main_path{ ProjectSettings::get_singleton()->get_setting("jank/main") };
    Ref<FileAccess> f{ FileAccess::open(main_path, FileAccess::READ) };
    if (f.is_valid()) {
      std::string const src{ "(do\n" +
                             std::string(f->get_as_text().utf8().get_data()) + "\n)" };
      safe_eval(src, "preload");
      UtilityFunctions::print("jank: preloaded ", main_path);
    }
  }

  if (repl_enabled()) {
    start_nrepl();
    UtilityFunctions::print("jank: runtime host up - nREPL enabled");
  } else {
    UtilityFunctions::print("jank: runtime host up - REPL disabled");
  }
}

void JankRuntime::_process(double) {
  if (Engine::get_singleton()->is_editor_hint()) { return; }
  if (g_signal_quit.load()) {
    if (SceneTree *st{ get_tree() }) { st->quit(); }
  }
}

String JankRuntime::eval(String const &code) {
  eval_on_jank(std::string(code.utf8().get_data()));
  return String("ok");
}

void JankRuntime::_notification(int p_what) {
  if (Engine::get_singleton()->is_editor_hint()) { return; }
  if (p_what == NOTIFICATION_WM_CLOSE_REQUEST ||
      p_what == NOTIFICATION_EXIT_TREE) {
    shutdown_jank();
  }
}

// ======================================================================
// JankNode - attach to a node; binds it to a jank ns. self = this node (i64)
// Calls (ns/ready self) once and (ns/process self delta) per frame, INLINE on the main
//   thread - so the jank code can read AND write the node synchronously via godot/*
// ======================================================================
void JankNode::_bind_methods() {
  ClassDB::bind_method(D_METHOD("set_jank_ns", "ns"), &JankNode::set_jank_ns);
  ClassDB::bind_method(D_METHOD("get_jank_ns"), &JankNode::get_jank_ns);
  ADD_PROPERTY(PropertyInfo(Variant::STRING, "jank_ns"), "set_jank_ns", "get_jank_ns");
  ClassDB::bind_method(D_METHOD("set_jank_script", "path"), &JankNode::set_jank_script);
  ClassDB::bind_method(D_METHOD("get_jank_script"), &JankNode::get_jank_script);
  ADD_PROPERTY(PropertyInfo(Variant::STRING, "jank_script", PROPERTY_HINT_FILE,
                            "*.jank"),
               "set_jank_script", "get_jank_script");
}

void JankNode::set_jank_ns(String const &ns) { ns_ = ns; }
String JankNode::get_jank_ns() const { return ns_; }
void JankNode::set_jank_script(String const &p) { script_ = p; }
String JankNode::get_jank_script() const { return script_; }

std::string JankNode::ns_utf8() const {
  return ns_.is_empty() ? std::string("user") : std::string(ns_.utf8().get_data());
}

void JankNode::_ready() {
  if (Engine::get_singleton()->is_editor_hint()) { return; }
  install_signal_bridge();
  if (!ensure_vm()) { return; }

  std::string const ns{ ns_utf8() };
  if (!script_.is_empty()) {
    Ref<FileAccess> f{ FileAccess::open(script_, FileAccess::READ) };
    if (f.is_valid()) {
      load_script_into_ns(ns, std::string(f->get_as_text().utf8().get_data()));
      UtilityFunctions::print("jank: JankNode loaded ", script_, " -> ns '", ns_, "'");
    } else {
      UtilityFunctions::printerr("jank: JankNode script not found: ", script_);
    }
  }
  loaded_ = true;
  call_node_fn(ns, "ready", reinterpret_cast<int64_t>(static_cast<Node2D *>(this)),
               false, 0.0);
}

void JankNode::_process(double delta) {
  if (Engine::get_singleton()->is_editor_hint()) { return; }
  if (g_signal_quit.load()) {
    if (SceneTree *st{ get_tree() }) { st->quit(); }
    return;
  }
  if (!loaded_) { return; }
  call_node_fn(ns_utf8(), "process",
               reinterpret_cast<int64_t>(static_cast<Node2D *>(this)), true, delta);
}

void JankNode::_notification(int p_what) {
  if (Engine::get_singleton()->is_editor_hint()) { return; }
  if (p_what == NOTIFICATION_WM_CLOSE_REQUEST ||
      p_what == NOTIFICATION_EXIT_TREE) {
    shutdown_jank();
  }
}
