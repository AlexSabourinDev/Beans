from generate_shader_headers import generateShaderHeader
from compile_shaders import compileShader
import os

gameDir = f"{os.path.dirname(os.path.abspath(__file__))}/../VoxelGame/"

def generateShader(input, output, outputHeader, type, entry):
    compileShader(f"{gameDir}/{input}", f"{gameDir}/{output}", type, entry)
    generateShaderHeader(f"{gameDir}/{output}", f"{gameDir}/{outputHeader}")

if not os.path.exists(f"{gameDir}/ShaderHeaders/"):
    os.mkdir(f"{gameDir}/ShaderHeaders/")

generateShader("Shaders/background.hlsl",
               "CompiledAssets/Shaders/background.spv",
               "ShaderHeaders/background.h",
               "compute",
               "CS")