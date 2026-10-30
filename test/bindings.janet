#!/usr/bin/env janet
#
# Smoke test for the ghostty Janet bindings. Does not open a window.
#
#   JANET_PATH=build LD_LIBRARY_PATH=vendor/ghostty/lib janet test/bindings.janet

(import ghostty :as g)

(defn check [label got expected]
  (if (= got expected)
    (print "ok   " label)
    (do
      (print "FAIL " label " expected " (string/format "%j" expected)
             " got " (string/format "%j" got))
      (os/exit 1))))

# --- build info -------------------------------------------------------------
(def info (g/build-info))
(check "build-info has :simd" (boolean? (get info :simd)) true)
(check "build-info has :optimize" (keyword? (get info :optimize)) true)

# --- key tables -------------------------------------------------------------
(check "keys table size" (length g/keys) 74)
(check "enter key code" (get g/keys :enter) 58)
(check "shift mod" (get g/mods :shift) 1)

# --- terminal + effects -----------------------------------------------------
(def t (g/terminal-new 80 24))
(g/terminal-resize t 80 24 8 16)

(var responses @[])
(g/terminal-set-effects t
  {:write-pty (fn [bytes] (array/push responses (string bytes)))})

# DA1 query: the terminal should answer through write-pty.
(g/terminal-write t "\e[c")
(check "DA1 response emitted" (> (length responses) 0) true)

# --- rendering --------------------------------------------------------------
(g/terminal-write t "hi \e[1;31mred\e[0m \e[3mital\e[0m")
(def r (g/render-new))
(g/render-update r t)
(def row (get (g/render-rows r) 0))
(check "plain text" (get (get row 0) :text) "h")
(check "bold flag" (get (get row 3) :bold) true)
(check "bold color resolved" (get (get row 3) :fg) [204 102 102])
(check "italic flag" (get (get row 10) :italic) true)

# --- key encoding -----------------------------------------------------------
(def enc (g/key-encoder-new))
(check "printable key uses text"
       (g/key-encode enc t (get g/keys :a) (get g/key-actions :press) 0 97 0 "a")
       "a")
(check "enter key"
       (g/key-encode enc t (get g/keys :enter) (get g/key-actions :press) 0 0 0 nil)
       "\r")
(check "arrow key"
       (g/key-encode enc t (get g/keys :up) (get g/key-actions :press) 0 0 0 nil)
       "\e[A")
(check "ctrl-c"
       (g/key-encode enc t (get g/keys :c) (get g/key-actions :press) (get g/mods :ctrl) 99 0 nil)
       "\x03")
(check "focus gained" (g/focus-encode true) "\e[I")

# --- pty round trip ---------------------------------------------------------
(def pty (g/pty-spawn 80 24 8 16 "/bin/sh"))
(def fd (pty :fd))
(def pid (pty :pid))
(g/pty-write fd "echo grassy-ok\n")
(var got "")
(def deadline (+ (os/time) 3))
(while (and (< (os/time) deadline) (not (string/find "grassy-ok" got)))
  (def chunk (g/pty-read fd))
  (when (buffer? chunk) (set got (string got chunk)))
  (os/sleep 0.02))
(check "pty round trip" (not (nil? (string/find "grassy-ok" got))) true)
(g/pty-kill pid 15)
(g/pty-close fd)

(print "all binding tests passed")
