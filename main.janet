#!/usr/bin/env janet
#
# grassy-terrain - a bare-minimum terminal emulator in Janet.
#
# libghostty-vt does all of the terminal emulation (parsing, screen state,
# reflow, scrollback, input encoding). Jaylib/raylib does windowing and 2D
# drawing. This file is the glue: a render loop, PTY plumbing, and input
# forwarding.
#
# Run it with the generated build/gt launcher (see scripts/build.sh).

(import jaylib :as rl)
(import ghostty :as g)

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

(def font-size 16)     # logical points
(def padding 4)        # pixels between the window edge and the grid
(def scrollback-lines 1000)
(def wheel-scroll-rows 3)

(def root (or (os/getenv "GRASSY_ROOT") "."))
(def font-path (string root "/assets/JetBrainsMono-Regular.ttf"))
(def jaylib-path (or (os/getenv "GRASSY_JAYLIB")
                     (string (or (os/getenv "HOME") ".") "/source/jaylib/build/jaylib.so")))

# ---------------------------------------------------------------------------
# Raylib FFI shims
#
# Jaylib exposes most of raylib, but its `get-char-pressed` wrapper has a
# typo (it reads argv[1] instead of argv[0]), so it cannot return raw
# codepoints. It also does not bind IsKeyPressedRepeat at all. raylib's
# symbols are exported from jaylib.so, so we reach them directly with the
# Janet FFI instead.
# ---------------------------------------------------------------------------

(def raylib-lib (ffi/native jaylib-path))
(def raylib-get-char (ffi/lookup raylib-lib "GetCharPressed"))
(def raylib-key-pressed (ffi/lookup raylib-lib "IsKeyPressed"))
(def raylib-key-repeat (ffi/lookup raylib-lib "IsKeyPressedRepeat"))
(def raylib-key-released (ffi/lookup raylib-lib "IsKeyReleased"))

(def sig-int (ffi/signature :default :int))
(def sig-bool-int (ffi/signature :default :bool :int))

(defn next-char [] (ffi/call raylib-get-char sig-int))
(defn key-pressed? [code] (ffi/call raylib-key-pressed sig-bool-int code))
(defn key-repeat? [code] (ffi/call raylib-key-repeat sig-bool-int code))
(defn key-released? [code] (ffi/call raylib-key-released sig-bool-int code))

# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

(defn utf8 [cp]
  "Encode a Unicode codepoint as a UTF-8 Janet string."
  (cond
    (< cp 0x80) (string/from-bytes cp)
    (< cp 0x800) (string/from-bytes
                   (bor 0xC0 (brshift cp 6))
                   (bor 0x80 (band cp 0x3F)))
    (< cp 0x10000) (string/from-bytes
                     (bor 0xE0 (brshift cp 12))
                     (bor 0x80 (band (brshift cp 6) 0x3F))
                     (bor 0x80 (band cp 0x3F)))
    (string/from-bytes
      (bor 0xF0 (brshift cp 18))
      (bor 0x80 (band (brshift cp 12) 0x3F))
      (bor 0x80 (band (brshift cp 6) 0x3F))
      (bor 0x80 (band cp 0x3F)))))

(defn rgb [c]
  "Convert an [r g b] byte tuple into a raylib float color."
  (if c
    [(/ (c 0) 255) (/ (c 1) 255) (/ (c 2) 255)]
    [1 1 1]))

(defn rgb-a [c a]
  (if c
    [(/ (c 0) 255) (/ (c 1) 255) (/ (c 2) 255) a]
    [1 1 1 a]))

# ---------------------------------------------------------------------------
# Keyboard map
#
# Each entry is [raylib-key-code ghostty-key-keyword unshifted-codepoint].
# The unshifted codepoint is what the key produces with no modifiers; the
# Kitty keyboard protocol needs it to identify keys independent of Shift.
# ---------------------------------------------------------------------------

(def key-map @[])
(for i 0 26
  (array/push key-map [(+ 65 i) (keyword (string/from-bytes (+ 97 i))) (+ 97 i)]))
(for i 0 10
  (array/push key-map [(+ 48 i) (keyword (string/from-bytes (+ 48 i))) (+ 48 i)]))
(each entry
  [[32 :space 32] [257 :enter 0] [258 :tab 0] [259 :backspace 0]
   [261 :delete 0] [256 :escape 0]
   [265 :up 0] [264 :down 0] [263 :left 0] [262 :right 0]
   [268 :home 0] [269 :end 0] [266 :page-up 0] [267 :page-down 0] [260 :insert 0]
   [45 (keyword "-") 45] [61 (keyword "=") 61]
   [91 (keyword "[") 91] [93 (keyword "]") 93] [92 (keyword "\\") 92]
   [59 (keyword ";") 59] [39 (keyword "'") 39] [44 (keyword ",") 44]
   [46 (keyword ".") 46] [47 (keyword "/") 47] [96 (keyword "`") 96]
   [290 :f1 0] [291 :f2 0] [292 :f3 0] [293 :f4 0] [294 :f5 0] [295 :f6 0]
   [296 :f7 0] [297 :f8 0] [298 :f9 0] [299 :f10 0] [300 :f11 0] [301 :f12 0]]
  (array/push key-map entry))

(def mouse-map
  [[:left :left] [:right :right] [:middle :middle]])

(defn get-mods []
  "Build the ghostty modifier bitmask from the current raylib key state."
  (bor
    (if (or (rl/key-down? :left-shift) (rl/key-down? :right-shift)) (get g/mods :shift) 0)
    (if (or (rl/key-down? :left-control) (rl/key-down? :right-control)) (get g/mods :ctrl) 0)
    (if (or (rl/key-down? :left-alt) (rl/key-down? :right-alt)) (get g/mods :alt) 0)
    (if (or (rl/key-down? :left-super) (rl/key-down? :right-super)) (get g/mods :super) 0)))

# ---------------------------------------------------------------------------
# Input handling
# ---------------------------------------------------------------------------

(defn handle-input [term kenc fd]
  "Translate raylib key events into VT sequences and write them to the PTY."
  # Drain printable characters first. raylib reports them separately from key
  # transitions, and libghostty's encoder needs the text for printable keys.
  (var char-text "")
  (var cp (next-char))
  (while (not= cp 0)
    (set char-text (string char-text (utf8 cp)))
    (set cp (next-char)))

  (def mods (get-mods))

  (each [code gkw ucp] key-map
    (def pressed (key-pressed? code))
    (def repeated (if pressed false (key-repeat? code)))
    (def released (key-released? code))
    (when (or pressed repeated released)
      (def action (if released
                    (get g/key-actions :release)
                    (if pressed (get g/key-actions :press) (get g/key-actions :repeat))))
      (def consumed (if (and (not= ucp 0) (not= 0 (band mods (get g/mods :shift))))
                     (get g/mods :shift) 0))
      # Attach pending text to the first non-release event this frame, matching
      # the reference implementation. Release events never carry text.
      (def text (if (and (not released) (not (empty? char-text))) char-text nil))
      (when text (set char-text ""))
      (def out (g/key-encode kenc term (get g/keys gkw) action mods ucp consumed text))
      (when out (g/pty-write fd out))))

  # Some platforms deliver the character a frame after the key press. If no
  # key event consumed the text, send it directly so input is not dropped.
  (when (not (empty? char-text))
    (g/pty-write fd char-text)))

(defn handle-mouse [term menc fd]
  "Forward mouse events through libghostty's mouse encoder."
  (def pos (rl/get-mouse-position))
  (def mx (pos 0))
  (def my (pos 1))
  (def delta (rl/get-mouse-delta))
  (def mods (get-mods))
  (def any-pressed (or (rl/mouse-button-down? :left)
                       (rl/mouse-button-down? :right)
                       (rl/mouse-button-down? :middle)))

  (each [rl-btn g-btn] mouse-map
    (when (rl/mouse-button-pressed? rl-btn)
      (def out (g/mouse-encode menc term (get g/mouse-actions :press)
                               (get g/mouse-buttons g-btn) mx my mods any-pressed))
      (when out (g/pty-write fd out)))
    (when (rl/mouse-button-released? rl-btn)
      (def out (g/mouse-encode menc term (get g/mouse-actions :release)
                               (get g/mouse-buttons g-btn) mx my mods any-pressed))
      (when out (g/pty-write fd out))))

  (when (or (not= (delta 0) 0) (not= (delta 1) 0))
    (def held (cond
                (rl/mouse-button-down? :left) (get g/mouse-buttons :left)
                (rl/mouse-button-down? :right) (get g/mouse-buttons :right)
                (rl/mouse-button-down? :middle) (get g/mouse-buttons :middle)
                0))
    (def out (g/mouse-encode menc term (get g/mouse-actions :motion) held mx my mods any-pressed))
    (when out (g/pty-write fd out)))

  # Wheel: forward to the application when it enabled mouse tracking,
  # otherwise scroll our own viewport through scrollback.
  (def wheel (rl/get-mouse-wheel-move))
  (when (not= wheel 0)
    (if (g/terminal-mouse-tracking? term)
      (do
        (def btn (get g/mouse-buttons (if (> wheel 0) :four :five)))
        (def press (g/mouse-encode menc term (get g/mouse-actions :press) btn mx my mods any-pressed))
        (when press (g/pty-write fd press))
        (def release (g/mouse-encode menc term (get g/mouse-actions :release) btn mx my mods any-pressed))
        (when release (g/pty-write fd release)))
      (g/terminal-scroll term (if (> wheel 0) (- wheel-scroll-rows) wheel-scroll-rows)))))

# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

(defn draw-scrollbar [term]
  (def sb (g/terminal-scrollbar term))
  (when (and sb (> (sb :total) (sb :len)))
    (def w (rl/get-screen-width))
    (def h (rl/get-screen-height))
    (def bar-w 6)
    (def margin 2)
    (def bar-x (- w bar-w margin))
    (def visible-frac (/ (sb :len) (sb :total)))
    (var thumb-h (math/floor (* h visible-frac)))
    (when (< thumb-h 10) (set thumb-h 10))
    (def scrollable (- (sb :total) (sb :len)))
    (def frac (if (> scrollable 0) (/ (sb :offset) scrollable) 1))
    (def thumb-y (math/floor (* frac (- h thumb-h))))
    (rl/draw-rectangle bar-x thumb-y bar-w thumb-h [0.78 0.78 0.78 0.5])))

(defn draw-banner [font message]
  (def size (rl/measure-text-ex font message font-size 0))
  (def sw (rl/get-screen-width))
  (def sh (rl/get-screen-height))
  (def bh (+ (size 1) 8))
  (rl/draw-rectangle 0 (- sh bh) sw bh [0 0 0 0.7])
  (rl/draw-text-ex font message
                   [(/ (- sw (size 0)) 2) (+ (- sh bh) 4)]
                   font-size 0 [1 1 1]))

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

(defn main [&]
  (rl/set-config-flags :window-highdpi)
  (rl/init-window 800 600 "grassy-terrain")
  (rl/set-window-state :window-resizable)
  (rl/set-target-fps 60)

  (def dpi (rl/get-window-scale-dpi))
  (def dpi-x (dpi 0))
  (def dpi-y (dpi 1))
  (def font-size-px (math/floor (* font-size dpi-y)))
  (def font (rl/load-font-ex font-path font-size-px))

  (def glyph (rl/measure-text-ex font "M" font-size-px 0))
  (def cell-w (max 1 (math/floor (/ (glyph 0) dpi-x))))
  (def cell-h (max 1 (math/floor (/ (glyph 1) dpi-y))))

  (def screen-w (rl/get-screen-width))
  (def screen-h (rl/get-screen-height))
  (defn grid-cols [w] (max 1 (math/floor (/ (- w (* 2 padding)) cell-w))))
  (defn grid-rows [h] (max 1 (math/floor (/ (- h (* 2 padding)) cell-h))))

  (var cols (grid-cols screen-w))
  (var rows (grid-rows screen-h))

  # --- ghostty terminal -----------------------------------------------------
  (def term (g/terminal-new cols rows))
  (g/terminal-set-scrollback term scrollback-lines)
  (g/terminal-resize term cols rows cell-w cell-h)

  # --- PTY ------------------------------------------------------------------
  (def pty (g/pty-spawn cols rows cell-w cell-h))
  (def fd (pty :fd))
  (def pid (pty :pid))

  # Effects: libghostty asks us to write query responses back to the PTY and
  # to update the window title. Device attributes, size reports, and XTVERSION
  # are handled natively inside the binding module.
  (g/terminal-set-effects term
    {:write-pty (fn [bytes] (g/pty-write fd bytes))
     :title-changed (fn [title] (rl/set-window-title title))})

  (def kenc (g/key-encoder-new))
  (def menc (g/mouse-encoder-new))
  (g/mouse-set-geometry menc screen-w screen-h cell-w cell-h padding)

  (def render (g/render-new))

  # --- loop state -----------------------------------------------------------
  (var prev-w screen-w)
  (var prev-h screen-h)
  (var prev-focused (rl/window-focused?))
  (var child-exited false)
  (var child-status -1)

  (while (not (rl/window-should-close))
    # Resize: reflow the terminal and tell the child about the new size.
    (when (rl/window-resized?)
      (def w (rl/get-screen-width))
      (def h (rl/get-screen-height))
      (when (or (not= w prev-w) (not= h prev-h))
        (set cols (grid-cols w))
        (set rows (grid-rows h))
        (g/terminal-resize term cols rows cell-w cell-h)
        (g/pty-resize fd cols rows cell-w cell-h)
        (g/mouse-set-geometry menc w h cell-w cell-h padding)
        (set prev-w w)
        (set prev-h h)))

    # Focus reporting (DECSET 1004), only if the application asked for it.
    (def focused (rl/window-focused?))
    (when (not= focused prev-focused)
      (when (and (not child-exited) (g/terminal-focus-event? term))
        (def out (g/focus-encode focused))
        (when out (g/pty-write fd out)))
      (set prev-focused focused))

    # Drain shell output into the terminal.
    (when (not child-exited)
      (def data (g/pty-read fd))
      (cond
        (= data :eof) (set child-exited true)
        (buffer? data) (g/terminal-write term data)
        nil nil))

    # Reap the child once it has exited.
    (when (and child-exited (< child-status 0))
      (def status (g/pty-wait pid))
      (when status (set child-status status)))

    # Input and rendering.
    (when (not child-exited)
      (handle-input term kenc fd)
      (handle-mouse term menc fd))

    (g/render-update render term)
    (def colors (g/render-colors render))
    (def default-bg (get colors :bg))
    (def default-fg (get colors :fg))

    (rl/begin-drawing)
    (rl/clear-background (rgb default-bg))

    (def grid (g/render-rows render))
    (var y padding)
    (each row grid
      (var x padding)
      (each cell row
        (var fg (or (get cell :fg) default-fg))
        (var bg (get cell :bg))
        (when (get cell :inverse)
          (def tmp fg)
          (set fg (or bg default-bg))
          (set bg tmp))
        (when bg
          (rl/draw-rectangle x y cell-w cell-h (rgb bg)))
        (def text (get cell :text))
        (when (and text (not (empty? text)))
          (def offset (if (get cell :italic) (math/floor (/ font-size 6)) 0))
          (rl/draw-text-ex font text [(+ x offset) y] font-size 0 (rgb fg))
          (when (get cell :bold)
            (rl/draw-text-ex font text [(+ x offset 1) y] font-size 0 (rgb fg))))
        (+= x cell-w))
      (+= y cell-h))

    (def cursor (g/render-cursor render))
    (when (and (cursor :visible) (cursor :has-value))
      (def cx (+ padding (* (cursor :x) cell-w)))
      (def cy (+ padding (* (cursor :y) cell-h)))
      (def cur-color (or (get colors :cursor) default-fg))
      (rl/draw-rectangle cx cy cell-w cell-h (rgb-a cur-color 0.5)))

    (draw-scrollbar term)

    (when child-exited
      (draw-banner font
        (if (>= child-status 0)
          (string "[process exited with status " child-status "]")
          "[process exited]")))

    (rl/end-drawing))

  # --- cleanup --------------------------------------------------------------
  (rl/unload-font font)
  (rl/close-window)
  (g/pty-close fd)
  (when (>= pid 0)
    (g/pty-kill pid 1)
    (var waited (g/pty-wait pid))
    (var tries 0)
    (while (and (nil? waited) (< tries 100))
      (os/sleep 0.01)
      (set waited (g/pty-wait pid))
      (+= tries 1))))

(main)
