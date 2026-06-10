@tool
extends EditorPlugin

# Enabling this plugin registers the JankRuntime autoload (the jank host singleton).
# The demo project registers the autoload directly in project.godot instead, so it
# also works headless without the editor; this is the convenience path for users.

const AUTOLOAD_NAME := "JankRuntime"
const AUTOLOAD_PATH := "res://addons/godot_jank/jank_runtime.tscn"


func _enter_tree() -> void:
	add_autoload_singleton(AUTOLOAD_NAME, AUTOLOAD_PATH)


func _exit_tree() -> void:
	remove_autoload_singleton(AUTOLOAD_NAME)
