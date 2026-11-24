from generate_shader_headers import generateShaderHeader
from compile_shaders import compileShader
import os
import argparse

gameDir = f"{os.path.dirname(os.path.abspath(__file__))}/../VoxelGame/"

def generateShader(input, output, outputHeader, type, entry, only_shader):
    compileShader(f"{gameDir}/{input}", f"{gameDir}/{output}", type, entry)
    if(not only_shader):
        generateShaderHeader(f"{gameDir}/{output}", f"{gameDir}/{outputHeader}")

shaders = [
    ("clear_color", "compute", "CS"),
    ("trace_voxels", "compute", "CS")]

if __name__ == "__main__":
    parser = argparse.ArgumentParser("Asset Compiler")
    parser.add_argument("-only_shaders", help="only recompile shaders (don't generate shader headers)", default=False)
    args = parser.parse_args()

    if not os.path.exists(f"{gameDir}/ShaderHeaders/"):
        os.mkdir(f"{gameDir}/ShaderHeaders/")

    for shader in shaders:
        generateShader(f"Shaders/{shader[0]}.hlsl",
                       f"CompiledAssets/Shaders/{shader[0]}.spv",
                       f"ShaderHeaders/{shader[0]}.h",
                       shader[1],
                       shader[2],
                       args.only_shaders)