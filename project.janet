(declare-project
  :name "grassy-terrain"
  :description "A minimal terminal emulator in Janet using libghostty-vt and Jaylib"
  :license "MIT"
  :url "https://github.com/example/grassy-terrain")

# The native module wraps libghostty-vt and the POSIX PTY helpers.
# `jpm build` produces build/ghostty.so.
(declare-native
  :name "ghostty"
  :source ["src/ghostty.c"]
  :cflags ["-Ivendor/ghostty/include" "-std=gnu11"]
  :lflags ["-Lvendor/ghostty/lib" "-lghostty-vt" "-lutil"])
