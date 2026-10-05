# libopus のソースは "celt.h" "main.h" のようにディレクトリ無しで相互 include するので、
# このライブラリ自身のコンパイルにだけ celt/ silk/ src/ を include パスへ足す。
# プロジェクト側からは include/opus.h だけが見えればよい(library.json の includeDir)。
import os

Import("env")

# SConscript として読まれるので __file__ は無い。Dir(".") がこのスクリプトのディレクトリ。
here = env.Dir(".").srcnode().abspath
if not os.path.isdir(os.path.join(here, "celt")):
    here = os.path.join(env.subst("$PROJECT_DIR"), "lib", "opus")
env.Append(CPPPATH=[os.path.join(here, d) for d in ("celt", "silk", "src")])
