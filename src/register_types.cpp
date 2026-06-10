#include "register_types.h"
#include "jank_runtime.h"
#include "jank_node.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

void initialize_jank_module(ModuleInitializationLevel p_level) {
  if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
    return;
  }
  ClassDB::register_class<JankRuntime>();
  ClassDB::register_class<JankNode>();
}

void uninitialize_jank_module(ModuleInitializationLevel p_level) {
  (void)p_level;
}

extern "C" {
GDExtensionBool GDE_EXPORT godot_jank_init(
    GDExtensionInterfaceGetProcAddress p_get_proc_address,
    const GDExtensionClassLibraryPtr p_library,
    GDExtensionInitialization *r_initialization) {
  GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library,
                                          r_initialization);
  init_obj.register_initializer(initialize_jank_module);
  init_obj.register_terminator(uninitialize_jank_module);
  init_obj.set_minimum_library_initialization_level(
      MODULE_INITIALIZATION_LEVEL_SCENE);
  return init_obj.init();
}
}
