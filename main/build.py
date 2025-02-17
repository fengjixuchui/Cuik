import glob
import os
import platform
import subprocess
import argparse

parser = argparse.ArgumentParser(description='Compiles TB')
parser.add_argument('--opt', action='store_true', help='runs optimize on compiled source')

args = parser.parse_args()
if args.opt:
	subprocess.check_call(['build.py', 'x64', 'aarch64', '--opt'], shell=True, cwd="../tilde-backend")
	subprocess.check_call(['build.py', '--usetb', '--opt'], shell=True, cwd="../libCuik")
else:
	subprocess.check_call(['build.py', 'x64', 'aarch64'], shell=True, cwd="../tilde-backend")
	subprocess.check_call(['build.py', '--usetb'], shell=True, cwd="../libCuik")

# link everything together
ninja = open('build.ninja', 'w')

cflags = "-g -Wall -Werror -Wno-unused-function"
cflags += " -I ../libCuik/include -I ../tilde-backend/include"
cflags += " -DCUIK_USE_TB -D_CRT_SECURE_NO_WARNINGS"

# windows' CRT doesn't support c11 threads so we provide a fallback
if platform.system() == "Windows":
	exe_ext = ".exe"
	cflags += " -I ../c11threads"
else:
	exe_ext = ""

# write some rules
ninja.write(f"""
cflags = {cflags}

rule cc
  depfile = $out.d
  command = clang $in $cflags -MD -MF $out.d -c -o $out
  description = CC $in $out

rule link
  command = clang $in -g -o $out
  description = LINK $out

""")

# compile source files
objs = []
list = glob.glob("src/*.c")

if platform.system() == "Windows":
	list.append("../c11threads/threads_msvc.c")

for f in list:
	obj = os.path.basename(f).replace('.c', '.o')
	ninja.write(f"build bin/{obj}: cc {f}\n")
	objs.append("bin/"+obj)

ninja.write(f"build cuik{exe_ext}: link {' '.join(objs)} ../libCuik/libcuik.lib ../tilde-backend/tildebackend.lib\n")
ninja.close()

subprocess.call(['ninja'])
