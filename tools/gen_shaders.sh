#!/bin/sh
# Regenerates the committed SPIR-V headers from the GLSL sources. Only needed
# when a shader changes; the build itself never invokes glslangValidator.
set -e
cd "$(dirname "$0")/.."

glslangValidator -V --vn blend_comp_spv \
    -o src/shaders/blend.comp.spv.h src/shaders/blend.comp
echo "regenerated src/shaders/blend.comp.spv.h"
