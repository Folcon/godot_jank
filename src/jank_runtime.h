#pragma once
#include <godot_cpp/classes/node.hpp>

namespace godot {

// Optional host singleton (add as an autoload). Boots the jank VM early, preloads shared jank
//   (project setting jank/main), and opt-in (jank/repl_enabled, default on), starts the nREPL
// Not required: a JankNode boots the VM lazily on its own
// Add JankRuntime when you want nREPL and to configure the boot or no hitch when JankNode first loads
class JankRuntime : public Node {
  GDCLASS(JankRuntime, Node)

protected:
  static void _bind_methods();

public:
  JankRuntime() = default;
  ~JankRuntime() override = default;

  void _ready() override;
  void _process(double delta) override;
  void _notification(int p_what);

  // Eval a jank form on the main Godot thread (also callable from GDScript)
  String eval(const String &code);
};

} // namespace godot
