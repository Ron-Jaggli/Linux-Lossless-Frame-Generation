#!/bin/sh
# Regenerates the committed SPIR-V headers from the GLSL sources. Needed
# only when a shader changes; glslangValidator is not a build dependency.
set -eu
cd "$(dirname "$0")/.."
glslangValidator -V --vn blend_comp_spv \
    src/shaders/blend.comp -o src/shaders/blend_comp_spv.h
echo "regenerated src/shaders/blend_comp_spv.h"
