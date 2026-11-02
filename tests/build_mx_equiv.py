"""Build build/bin/mx_equiv (tests/mx_equiv.c + runtime/gfx/mx_translate.c + vk_shaderc_shim.c) with clang, libvulkan and shaderc.

    python tests/build_mx_equiv.py
    build/bin/mx_equiv <folder of vk_prog_*_1.bin> [max]
"""
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402
out = T.build('bin', 'mx_equiv')
os.makedirs(os.path.dirname(out), exist_ok=True)
pkgs = subprocess.run(['pkg-config', '--cflags', '--libs', 'vulkan', 'shaderc'], capture_output=True, text=True)
if pkgs.returncode:
    sys.exit('pkg-config vulkan shaderc: %s (sudo pacman -S vulkan-headers vulkan-icd-loader shaderc)' % pkgs.stderr.strip())
cmd = [T.CLANG, '-O1', '-w', '-D_GNU_SOURCE'] + T.runtime_includes() + [
       os.path.join(REPO, 'tests', 'mx_equiv.c'), T.runtime('gfx', 'mx_translate.c'), T.runtime('gfx', 'vk_shaderc_shim.c'),
       '-o', out] + pkgs.stdout.split() + ['-lpthread', '-lm']
r = subprocess.run(cmd, capture_output=True, text=True)
print((r.stdout + r.stderr)[-4000:])
print('built %s' % out if r.returncode == 0 else 'FAILED')
sys.exit(r.returncode)
