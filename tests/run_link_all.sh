#!/bin/sh
# Cross-module integration check (no unit tests, pure compile + link). Run from anywhere; exit 1 on
# any failure. Four passes:
#
# 1. Every header under src/{core,dsp,engine,gen,io,ui,rec} and desktop/posix_fs.h compiles on its own
#    (a header that relies on what an earlier include happened to pull in fails here).
# 2. EVERY .cpp under src/dsp, src/gen, src/io plus firmware/desktop/posix_fs.cpp is compiled with
#    the test flags (clang++ -c each) and all the objects are linked into a trivial main that
#    references nothing. Objects linked directly (not from an archive) are all kept, so a symbol
#    defined in two modules (duplicate / ODR problem) or referenced but defined nowhere (missing
#    definition) is a hard link error even when no test exercises it. tests/stubs_*.cpp are never
#    part of this link. The same link is then repeated with src/engine added, and once more with
#    src/ui and src/rec (the Ui and the session recorder, SPEC §9.23) on top of the engine.
# 3. ODR sniff for what the linker cannot see: every file is compiled again at -O0 (inline
#    functions are then emitted as weak symbols instead of being inlined away) and each weak
#    function defined in two or more objects must disassemble identically in all of them; a
#    differing body means two headers (or a header and a .cpp) define the same inline function
#    differently, which the linker would silently resolve to whichever copy comes first.
# 4. When the Arduino esp32 core's Xtensa toolchain is installed (~/Library/Arduino15, override
#    with LP_XTENSA_BIN=<dir with xtensa-esp32s3-elf-g++>): every firmware .cpp (not posix_fs; ui and rec included) is
#    compiled with the core's own C++ flags (gnu++2b, -mlongcalls, -fstack-protector, ...), the
#    objects are partially linked (duplicates), every undefined symbol must come from newlib /
#    libgcc / libstdc++ or the ESP-IDF runtime (__atomic_*, __stack_chk_*), and the largest
#    -fstack-usage frame must stay below 2 KB (ESP32 hard constraint). GCC warnings are printed.
cd "$(dirname "$0")/.." || exit 1
SRC=firmware/liveplayer/src
CXX="${CXX:-clang++}"
FLAGS="-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror=return-type -I$SRC"
TMP="${TMPDIR:-/tmp}"
OBJ="$TMP/lp_link_all_obj"
rm -rf "$OBJ" && mkdir -p "$OBJ/o2" "$OBJ/o0" "$OBJ/hdr" "$OBJ/sym" "$OBJ/xt" || exit 1
fail=0

FW=$(find $SRC/dsp $SRC/gen $SRC/io -name '*.cpp' | LC_ALL=C sort)   # firmware modules under test
CORE="$FW firmware/desktop/posix_fs.cpp"                               # + the host file system
ENGINE=$(find $SRC/engine -name '*.cpp' | LC_ALL=C sort)
UI=$(find $SRC/ui $SRC/rec -name '*.cpp' | LC_ALL=C sort)             # the Ui and the session recorder (SPEC §9.23) need the engine: linked in the second pass
HEADERS=$(find $SRC/core $SRC/dsp $SRC/engine $SRC/gen $SRC/io $SRC/ui $SRC/rec firmware/desktop -name '*.h' | LC_ALL=C sort)

objname() { printf '%s' "$1" | tr '/' '_' | sed 's/\.cpp$/.o/'; }

# --- 1. headers compile standalone -------------------------------------------------------------
nh=0
for h in $HEADERS; do
  tu="$OBJ/hdr/$(objname "$h").cpp"
  printf '#include "%s"\n' "$(pwd)/$h" > "$tu"
  if out=$($CXX $FLAGS -Wno-unused-function -c "$tu" -o "$tu.o" 2>&1); then nh=$((nh + 1)); else
    echo "HEADER FAIL: $h"; echo "$out"; fail=1; fi
done
echo "headers compiling standalone: $nh of $(echo $HEADERS | wc -w | tr -d ' ')"

# --- 2. compile every module object, link into a trivial main ------------------------------------
n=0
for f in $CORE $ENGINE $UI; do
  if out=$($CXX $FLAGS -c "$f" -o "$OBJ/o2/$(objname "$f")" 2>&1); then
    n=$((n + 1)); [ -n "$out" ] && { echo "warnings in $f:"; echo "$out"; }
  else echo "COMPILE FAIL: $f"; echo "$out"; fail=1; fi
done
echo "compiled $n objects at -O2"
[ $fail -ne 0 ] && exit 1

printf 'int main() { return 0; }\n' > "$OBJ/main.cpp"
$CXX $FLAGS -c "$OBJ/main.cpp" -o "$OBJ/main.o" || exit 1
objs_of() { for f in "$@"; do printf '%s ' "$OBJ/o2/$(objname "$f")"; done; }

if $CXX $(objs_of $CORE) "$OBJ/main.o" -o "$TMP/lp_link_all"; then
  echo "link dsp+gen+io+posix_fs: OK ($(echo $CORE | wc -w | tr -d ' ') objects)"
else echo "link dsp+gen+io+posix_fs: FAILED"; fail=1; fi

if $CXX $(objs_of $CORE $ENGINE) "$OBJ/main.o" -o "$TMP/lp_link_all_engine"; then
  echo "link dsp+gen+io+posix_fs+engine: OK"
else echo "link dsp+gen+io+posix_fs+engine: FAILED"; fail=1; fi

if $CXX $(objs_of $CORE $ENGINE $UI) "$OBJ/main.o" -o "$TMP/lp_link_all_ui"; then
  echo "link dsp+gen+io+posix_fs+engine+ui+rec: OK ($(echo $UI | wc -w | tr -d ' ') more objects)"
else echo "link dsp+gen+io+posix_fs+engine+ui+rec: FAILED"; fail=1; fi

# --- 3. ODR sniff on -O0 weak functions ---------------------------------------------------------
# Weak text symbols of one object: Mach-O via `nm -m` ("weak external"), ELF via the W/V type.
weak_syms() {
  if [ "$(uname)" = Darwin ]; then nm -m "$1" | awk '/\(__TEXT,__text\) weak external/ { print $NF }'
  else nm --defined-only "$1" | awk '$2 ~ /^[WV]$/ { print $3 }'; fi
}
if objdump --help 2>&1 | grep -q 'disassemble-symbols'; then
  for f in $CORE $ENGINE $UI; do
    o="$OBJ/o0/$(objname "$f")"
    $CXX -std=c++17 -O0 -I$SRC -c "$f" -o "$o" || { fail=1; continue; }
    syms=$(weak_syms "$o" | LC_ALL=C sort -u | paste -s -d, -)
    [ -z "$syms" ] && continue
    # One objdump per object; split the listing at each "<symbol>:" header into sym/<symbol>.<object>.
    # Normalise: drop addresses, encodings and relocation targets so only mnemonics + immediates remain.
    objdump -d --disassemble-symbols="$syms" "$o" \
      | sed -E 's/^[0-9a-f]+ <([^>]*)>:$/@@\1/; s/^ *[0-9a-f]+:[[:space:]]*//; s/^([0-9a-f]{2} ?){2,8}[[:space:]]+//; s/0x[0-9a-f]+ <[^>]*>/ADDR/g; s/<[^>]*>/SYM/g' \
      | awk -v dir="$OBJ/sym" -v obj="$(objname "$f")" '/^@@/ { out = dir "/" substr($0, 3) "." obj; next } out != "" && NF > 0 { print > out }'
  done
  nsym=0; nbad=0
  for first in "$OBJ"/sym/*; do
    [ -f "$first" ] || continue
    sym=${first##*/}; sym=${sym%%.firmware_*}
    case "$first" in *"$sym".firmware_*) ;; *) continue ;; esac
    set -- "$OBJ"/sym/"$sym".firmware_*
    [ $# -lt 2 ] && continue
    [ "$1" != "$first" ] && continue       # visit each symbol once (at its first file)
    nsym=$((nsym + 1))
    for other in "$@"; do
      if ! cmp -s "$first" "$other"; then
        nbad=$((nbad + 1)); fail=1
        echo "ODR: $(printf '%s' "$sym" | c++filt) differs between ${first##*.firmware_} and ${other##*.firmware_}"
        break
      fi
    done
  done
  echo "ODR sniff: $nsym inline functions shared by 2+ objects, $nbad differing"
else
  echo "ODR sniff skipped: objdump without --disassemble-symbols (llvm-objdump needed)"
fi

# --- 4. real target: Xtensa ESP32-S3 GCC with the Arduino core's flags ----------------------------
ARD="$HOME/Library/Arduino15/packages/esp32/tools"
XT_BIN="${LP_XTENSA_BIN:-$(ls -d "$ARD"/esp-x32/*/bin 2>/dev/null | tail -1)}"
XG="$XT_BIN/xtensa-esp32s3-elf-g++"
if [ -x "$XG" ]; then
  XNM="$XT_BIN/xtensa-esp32s3-elf-nm"; XLD="$XT_BIN/xtensa-esp32s3-elf-ld"; XSIZE="$XT_BIN/xtensa-esp32s3-elf-size"
  XFF=$(ls "$ARD"/esp32s3-libs/*/flags/cpp_flags 2>/dev/null | tail -1)
  if [ -f "$XFF" ]; then   # the core's own C++ flags minus defines, includes, debug info and the superseded -std
    XF=$(tr ' ' '\n' < "$XFF" | grep -vE '^-D|^-I|^@|^$|^-std=gnu\+\+2a$|^-g' | LC_ALL=C sort -u | tr '\n' ' ')
  else
    XF="-std=gnu++2b -mlongcalls -fno-rtti -fexceptions -fstack-protector -mdisable-hardware-atomics -ffunction-sections -fdata-sections -fno-jump-tables -fstrict-volatile-bitfields -Wno-unused-parameter"
  fi
  nxt=0; nwarn=0
  for f in $FW $ENGINE $UI; do
    o="$OBJ/xt/$(objname "$f")"
    if out=$($XG $XF -Os -Wall -Wextra -Werror=return-type -fstack-usage -I$SRC -c "$f" -o "$o" 2>&1); then nxt=$((nxt + 1)); else
      echo "TARGET COMPILE FAIL: $f"; echo "$out"; fail=1; continue; fi
    w=$(printf '%s\n' "$out" | grep -c 'warning:')
    [ "$w" -gt 0 ] && { nwarn=$((nwarn + w)); printf '%s\n' "$out" | grep 'warning:' | sed 's/^/  /'; }
  done
  echo "target xtensa-esp32s3 ($(basename "$(dirname "$XT_BIN")"), core flags): compiled $nxt objects, $nwarn warnings"
  if [ $nxt -gt 0 ]; then
    if $XLD -r "$OBJ"/xt/*.o -o "$OBJ/xt_all.o"; then
      $XNM -u "$OBJ/xt_all.o" | awk '{ print $2 }' | LC_ALL=C sort -u > "$OBJ/xt_undef.txt"
      for l in libc.a libm.a libgcc.a libstdc++.a libsupc++.a; do
        a=$($XG -mlongcalls -print-file-name=$l); [ -f "$a" ] && $XNM --defined-only "$a" 2>/dev/null | awk 'NF == 3 { print $3 }'
      done | LC_ALL=C sort -u > "$OBJ/xt_libdefs.txt"
      missing=$(LC_ALL=C comm -23 "$OBJ/xt_undef.txt" "$OBJ/xt_libdefs.txt" | grep -vE '^__(atomic|sync|stack_chk)_')
      if [ -n "$missing" ]; then echo "target link: symbols defined nowhere (not in newlib/libgcc/libstdc++/IDF runtime):"; echo "$missing" | sed 's/^/  /'; fail=1
      else echo "target partial link: OK, $(wc -l < "$OBJ/xt_undef.txt" | tr -d ' ') externals all provided by newlib/libgcc/libstdc++/IDF"; fi
    else echo "target partial link: FAILED (duplicate symbols)"; fail=1; fi
    top=$(cat "$OBJ"/xt/*.su 2>/dev/null | sort -t "$(printf '\t')" -k2 -n -r | head -1)
    frame=$(printf '%s' "$top" | awk -F '\t' '{ print $2 }')
    if [ -n "$frame" ] && [ "$frame" -ge 2048 ]; then echo "STACK FRAME >= 2 KB: $top"; fail=1
    else echo "largest target stack frame: ${frame:-?} B ($(printf '%s' "$top" | cut -f1 | sed 's/:[0-9]*:[0-9]*:/ /'))"; fi
    echo "target sizes: $($XSIZE -t "$OBJ"/xt/*.o | tail -1 | awk '{ print "text " $1 " B, data " $2 " B, bss " $3 " B" }')"
  fi
else
  echo "target pass skipped: Xtensa toolchain not found (set LP_XTENSA_BIN)"
fi

[ $fail -eq 0 ] && echo "run_link_all: PASS"
exit $fail
