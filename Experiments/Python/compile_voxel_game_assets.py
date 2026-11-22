from generate_shader_headers import generateShaderHeader
from compile_shaders import compileShader
import os

gameDir = f"{os.path.dirname(os.path.abspath(__file__))}/../VoxelGame/"

def generateShader(input, output, outputHeader, type, entry):
    compileShader(f"{gameDir}/{input}", f"{gameDir}/{output}", type, entry)
    generateShaderHeader(f"{gameDir}/{output}", f"{gameDir}/{outputHeader}")

if not os.path.exists(f"{gameDir}/ShaderHeaders/"):
    os.mkdir(f"{gameDir}/ShaderHeaders/")

shaders = [("clear_color", "compute", "CS")]

for shader in shaders:
    generateShader(f"Shaders/{shader[0]}.hlsl",
                   f"CompiledAssets/Shaders/{shader[0]}.spv",
                   f"ShaderHeaders/{shader[0]}.h",
                   shader[1],
                   shader[2])