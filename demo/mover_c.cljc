(ns mover-c
  (:require [godot]))

(defn process [self delta]
  (let [x (godot/get-x self "position")]
    (godot/set-position self (if (>= x 700.0) 0.0 (+ x 4.0)) 360)))
