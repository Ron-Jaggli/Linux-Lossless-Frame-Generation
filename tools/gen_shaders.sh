#!/bin/sh
# Regenerates the committed SPIR-V headers from the GLSL sources. Run after
# editing anything in src/shaders/; requires glslangValidator (Fedora:
# glslang, Ubuntu: glslang-tools). Committing the output keeps glslang out
# of the build-time dependencies.
set -e
cd "$(dirname "$0")/.."
glslangValidator -V --vn blend_comp_spv \
    -o src/shaders/blend_comp_spv.h src/shaders/blend.comp
echo "OK: src/shaders/blend_comp_spv.h"
