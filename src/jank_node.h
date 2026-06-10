#pragma once
#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/core/defs.hpp>   // GDE_EXPORT
#include <string>

namespace godot {

// Attach to a node to give it jank behaviour (the Arcadia-style hook)
// Binds the node to a jank namespace (`jank_ns`) loaded from a res:// .jank file (`jank_script`)
// `self` (this node) is passed to jank as an i64; the node is driven imperatively from jank via
//   the godot/* bridge helpers (e.g. (godot/set-position self x y))
// Boots the VM lazily on first use, no JankRuntime autoload required, however add it to remove the hitch on first load
class GDE_EXPORT JankNode : public Node2D {  // GDE_EXPORT: export the vtable/typeinfo so Godot's dlopen resolves them on Linux
  GDCLASS(JankNode, Node2D)

protected:
  static void _bind_methods();

public:
  JankNode() = default;
  ~JankNode() override = default;

  void _ready() override;
  void _process(double delta) override;
  void _notification(int p_what);

  void set_jank_ns(const String &ns);
  String get_jank_ns() const;
  void set_jank_script(const String &path);
  String get_jank_script() const;

private:
  std::string ns_utf8() const;

  String ns_;
  String script_;
  bool loaded_{ false };
};

} // namespace godot
